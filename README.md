# NetTune

Плагин для выделенного сервера **ARK: Survival Evolved** (клиент/сервер v361,
x64, ArkApi / `version.dll`). Ускоряет доставку мира клиенту (в первую очередь
больших баз/структур) и даёт инструменты для измерения и адаптации.

Написан и проверен на реальном сервере (v361, `ShooterGameServer.exe`),
все утверждения ниже сверены с бинарём (Ghidra + декомпилированный дамп) и
с живым рантаймом.

---

## Что реально влияет на скорость репликации (и что — нет)

Это ключевой раздел: серверный код ARK сильно модифицирован, и «очевидные»
рычаги (бюджет байт/сек) работают не так, как в ванильном UE4.

### 1. `UNetDriver::NetServerMaxTickRate` — это FPS сервера

`UGameEngine::GetMaxTickRate` читает `NetDriver+0x48` (это и есть
`NetServerMaxTickRate`) и клампит его в `[10, 120]`; `r.MaxFPS` перебивает.
Значит это реальная частота тиков движка, а вместе с ней — частота
`UNetDriver::TickFlush` → `ServerReplicateActors` + `FlushNet`.

Чем выше — тем больше пакетов/сек на соединение (и тем выше нагрузка на CPU).
**Прямой рычаг.**

### 2. `UPlayer::CurrentNetSpeed` — это БЮДЖЕТ НОВЫХ АКТОРОВ, а не байт/сек

В `UNetDriver::ServerReplicateActors` на каждый **новый** актор копится
`Connection->DormancyRate`:

```
f = clamp((CurrentNetSpeed - 14000) / 46000, 0, 1)
DormancyRate += (1.0 / (f*195 + 30)) / DeltaSeconds / GDormancyLimitMultiplier
```

`DormancyRate` убывает на 1.0 за тик. Чем меньше инкремент — тем больше новых
акторов сервер успевает отправить за тик. Итоговая скорость новых акторов
(в секунду) примерно:

```
new_actors_per_sec ≈ (f*195 + 30) * GDormancyLimitMultiplier
```

* при `CurrentNetSpeed <= 14000` (f=0): ~30 новых акторов/с;
* при `CurrentNetSpeed >= 60000` (f=1): ~225 новых акторов/с (при multiplier=1).

**ВАЖНО:** `f` насыщается на 1.0 при `CurrentNetSpeed >= 60000`, поэтому
значения **выше 60000 не дают ничего**. Полезный диапазон — `[14000, 60000]`.

Именно это поле присылает клиент: его настройка качества сети
(`ClientNetQuality` 0/1/2/3) превращается в `r.ClientDesiredNetSpeed`
(14000/20000/40000/60000) и уходит control-сообщением `NMT_Netspeed`; сервер
принимает его в `UWorld::NotifyControlMessage` (case 4) и клампит в тот же
диапазон. Поэтому «поставить всем 60000» = выжать максимум из этого механизма,
даже если у клиента выставлено «Low».

### 3. `GDormancyLimitMultiplier` (глобал, дефолт `1.0`)

Делитель инкремента выше. Больше => больше новых акторов за тик (пропорционально).
Раньше задавался только командной строкой `-DormancyNetMultiplier=<x>`;
плагин умеет менять на лету.

### 4. `GNoDormancyLimit` (глобал, дефолт `false`)

Полностью снимает троттлинг (то же, что `-nodormancythrottling`).
«Ядерный» вариант: без ограничения на новых акторов за тик.

### Чего НЕ делает (чтобы не обманываться)

* `CurrentNetSpeed` **не** является жёстким лимитом байт/сек: у
  `UNetConnection::IsNetReady` (проверка `QueuedBytes`) нет ни одного прямого
  вызова (Ghidra: `caller_count = 0`; есть только ссылки из таблиц функций), и
  на живом сервере `QueuedBytes` держится `<= 0`. Поэтому `CurrentNetSpeed`
  работает именно как dormancy-бюджет из п.2, а не как лимит байт/сек.
  (Гейта по байтам в коде не найдено; если он всё же есть, польза от него
  всё равно упирается в тот же диапазон `[14000, 60000]`.)
* `MaxClientRate` читается в `UWorld::Listen`, который жёстко ставит себе
  `60000`; на уже идущий поток влияет слабо.
* `AGameNetworkManager` (`TotalNetBandwidth`/`MaxDynamicBandwidth`/…) в
  отправке не участвует.
* `UNetConnection::OutBytes/InBytes/InPacketsLost/OutPacketsLost` **сбрасываются
  раз в `StatPeriod` (1 c)** — т.е. это уже счётчики «за секунду».

### Итог: как ускорить загрузку базы

1. `CurrentNetSpeed` = `60000` для всех (максимум бюджета новых акторов). Даёт
   до ~7.5× относительно «Low»-клиента.
2. `NetServerMaxTickRate` — баланс CPU/скорости (для 10 серверов на машине
   разумно 15–20).
3. `GDormancyLimitMultiplier` — дополнительный множитель (если есть запас по
   каналу и CPU).
4. Первичная отправка актора не гейтится интервалом `GetClientReplicationRateFor`
   (тот влияет только на повторные апдейты), поэтому на «догрузку базы» он не
   влияет.

---

## Измерения на живом сервере (v361, 1 игрок у большой базы)

Настройки: `NetServerMaxTickRate=20`, `CurrentNetSpeed=60000`, `MaxPacket=512`.

| Метрика | Значение |
|---|---|
| `peakOutBps` (пик отдачи клиенту) | **~296 КБ/с** |
| `peakDorm` (пик `DormancyRate`) | 0.076 |
| `queued` (бэклог) | `<= 0` (на полу) |
| `inLost` / `outLost` | 0 |
| `avglag` | ~0.25–0.39 с |

Что это значит:

* сервер отдаёт одному клиенту **~296 КБ/с** — в ~5 раз больше «бюджета»
  `60000`, т.е. `CurrentNetSpeed` **не** является лимитом байт/сек;
* `DormancyRate` не доходил и до `0.1` → при `tickrate 20` dormancy-троттлинг
  новых акторов **не срабатывает** (запас есть);
* `queued <= 0` → обратного давления нет.

Вывод: при таких настройках загрузка ограничена объёмом данных и частотой
тиков, а не бюджетом. `NetServerMaxTickRate` — единственный подтверждённый
прямой рычаг; `CurrentNetSpeed` держать `>= 60000` (запас на медленные
соединения), выше — смысла нет.

---

## Режимы

* `static` — раз в `applyIntervalSeconds` выставлять фиксированные значения.
* `adaptive` (по умолчанию) — на каждое соединение цель в
  `[AdaptiveMin, AdaptiveMax]`, старт с максимума; шаг вниз при потерях/лаге,
  шаг вверх при чистом канале. Выше `60000` смысла нет (см. п.2), поэтому
  `AdaptiveMax` по умолчанию `60000`.

Всё применяется **по таймеру**, потому что:

* клиент перезаписывает `CurrentNetSpeed` при смене своей настройки качества;
* `UWorld::Listen` перезаписывает `MaxClientRate=60000` при загрузке карты;
* новые соединения/структуры появляются со временем.

---

## Конфиг (`NetTune.json`, рядом с DLL)

```json
{
  "enabled": true,
  "mode": "adaptive",
  "applyIntervalSeconds": 10.0,
  "statusIntervalSeconds": 30.0,

  "NetServerMaxTickRate": 20,
  "MaxClientRate": 60000,
  "MaxInternetClientRate": 60000,

  "CurrentNetSpeed": 60000,
  "ConfiguredInternetSpeed": 60000,
  "ConfiguredLanSpeed": 60000,

  "AdaptiveMin": 14000,
  "AdaptiveMax": 60000,
  "AdaptiveStepUp": 10000,
  "AdaptiveStepDown": 15000,
  "AdaptiveLostPackets": 2,
  "AdaptiveLagThreshold": 0.0,
  "AdaptiveEvaluateSeconds": 10.0,

  "DormancyLimitMultiplier": 0.0,
  "DisableDormancyThrottling": false,

  "StructureReplicationIntervalMultiplier": 0.0,
  "StructureNetUpdateFrequency": 0.0,
  "StructureApplyIntervalSeconds": 60.0
}
```

* `0` в числовых полях = «не трогать».
* `AdaptiveLostPackets` — порог потерянных пакетов **за секунду** (по максимуму
  за окно), выше которого значение понижается.
* `AdaptiveLagThreshold` — порог `AvgLag` (секунды); `0` = не учитывать лаг.
* `DormancyLimitMultiplier` — глобал `GDormancyLimitMultiplier` (0 = оставить).
* `Structure*` — опциональная правка структур (на первичную загрузку не влияет).

## RCON-команды

| Команда | Действие |
|---|---|
| `NetTune.status` | вывести текущее состояние (driver, соединения, пики) |
| `NetTune.reload` | перечитать конфиг |
| `NetTune.on` / `NetTune.off` | включить/выключить применение |

`status` печатает по соединению: `netspeed`, `queued`, `maxpacket`,
`dorm`/`peakDorm`, `outBps`/`peakOutBps`, `avglag`, потери.

---

## Сборка

Требуется ArkApi и MSVC (x64). `NetTune.vcxproj` ожидает соседнюю папку
`ARK-Server-API` (как в дереве `ARK-Server-Plugins`):

```
.../ARK-Server-Plugins/NetTune/src/NetTune.vcxproj   -> ../../.. = .../ARK-Server-API
```

```
MSBuild src\NetTune.vcxproj /p:Configuration=Release /p:Platform=x64
```

Установка: положить DLL и `PluginInfo.json` в
`ShooterGame/Binaries/Win64/ArkApi/Plugins/NetTune/`
(имя DLL — `NetTune.dll`), при необходимости рядом `NetTune.json`.

---

## Рекомендации для 10 серверов на одной машине, 200 Мбит/с, ~20 игроков/карта

* 200 Мбит/с / 10 = 20 Мбит/с на сервер ≈ 2.5 МБ/с; 20 игроков × 60 КБ/с
  (максимум полезного `CurrentNetSpeed`) = 1.2 МБ/с ≈ 9.6 Мбит/с — с запасом.
* `NetServerMaxTickRate = 15..20` (в конфиге по умолчанию `20`) — экономит CPU
  и остаётся достаточным: при `CurrentNetSpeed=60000` это ~225 новых
  структур/с на игрока (база из ~500 объектов догружается за считанные секунды).
* `CurrentNetSpeed`/`MaxClientRate` = `60000` (потолок полезного).
* `DormancyLimitMultiplier` — держать `0` (оставить 1.0), поднимать только если
  есть запас канала/CPU и нужно ещё быстрее.
