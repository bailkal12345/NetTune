#pragma once

// ArkApi resolves a field name to an offset with a string lookup on every call. The
// offset is fixed for the binary, so it is resolved once from the first live object
// and reused; later reads use plain pointer arithmetic.

#include <API/ARK/Ark.h>

class NativeBase
{
public:
	explicit NativeBase(const char* name)
		: name_(name) {
	}

	bool IsResolved() const { return state_ == State::Resolved; }
	bool IsFailed() const { return state_ == State::Failed; }
	const char* Name() const { return name_; }

protected:
	enum class State { Unresolved, Resolved, Failed };

	const char* name_;
	State state_ = State::Unresolved;
};

template <typename T>
class NativeField : public NativeBase
{
public:
	explicit NativeField(const char* name)
		: NativeBase(name) {
	}

	T* Get(void* object)
	{
		if (object && state_ == State::Unresolved) {
			Resolve(object);
		}
		if (!object || state_ != State::Resolved) {
			return nullptr;
		}
		return reinterpret_cast<T*>(static_cast<char*>(object) + offset_);
	}

	// Writes the value only if it differs from the current one.
	// Returns true if write occurred.
	bool Set(void* object, const T& value)
	{
		T* pointer = Get(object);
		if (!pointer || *pointer == value) {
			return false;
		}
		*pointer = value;
		return true;
	}

private:
	void Resolve(const void* object)
	{
		const char* base = static_cast<const char*>(object);
		const char* field = GetNativePointerField<char*>(object, name_);
		if (field && field != base) {
			offset_ = field - base;
			state_ = State::Resolved;
		}
		else {
			state_ = State::Failed;
		}
	}

	ptrdiff_t offset_ = 0;
};

template <typename T>
class NativeGlobal : public NativeBase
{
public:
	explicit NativeGlobal(const char* name)
		: NativeBase(name) {
	}

	T* Get()
	{
		if (state_ == State::Unresolved) {
			value_ = GetNativeDataPointerField<T*>(name_);
			state_ = value_ ? State::Resolved : State::Failed;
		}
		return value_;
	}

	// Writes the value only if it differs from the current one.
	// Returns true if write occurred.
	bool Set(const T& value)
	{
		T* pointer = Get();
		if (!pointer || *pointer == value) {
			return false;
		}
		*pointer = value;
		return true;
	}

private:
	T* value_ = nullptr;
};