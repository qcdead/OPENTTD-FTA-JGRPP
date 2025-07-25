/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file bitmath_func.hpp Functions related to bit mathematics. */

#ifndef BITMATH_FUNC_HPP
#define BITMATH_FUNC_HPP

#include <bit>
#include <limits>
#include <type_traits>

/**
 * Fetch \a n bits from \a x, started at bit \a s.
 *
 * This function can be used to fetch \a n bits from the value \a x. The
 * \a s value set the start position to read. The start position is
 * count from the LSB and starts at \c 0. The result starts at a
 * LSB, as this isn't just an and-bitmask but also some
 * bit-shifting operations. GB(0xFF, 2, 1) will so
 * return 0x01 (0000 0001) instead of
 * 0x04 (0000 0100).
 *
 * @param x The value to read some bits.
 * @param s The start position to read some bits.
 * @param n The number of bits to read.
 * @pre n < sizeof(T) * 8
 * @pre s + n <= sizeof(T) * 8
 * @return The selected bits, aligned to a LSB.
 */
template <typename T>
debug_inline constexpr static uint GB(const T x, const uint8_t s, const uint8_t n)
{
	return (x >> s) & (((T)1U << n) - 1);
}

/**
 * Set \a n bits in \a x starting at bit \a s to \a d
 *
 * This function sets \a n bits from \a x which started as bit \a s to the value of
 * \a d. The parameters \a x, \a s and \a n works the same as the parameters of
 * #GB. The result is saved in \a x again. Unused bits in the window
 * provided by n are set to 0 if the value of \a d isn't "big" enough.
 * This is not a bug, its a feature.
 *
 * @note Parameter \a x must be a variable as the result is saved there.
 * @note To avoid unexpected results the value of \a d should not use more
 *       space as the provided space of \a n bits (log2)
 * @param x The variable to change some bits
 * @param s The start position for the new bits
 * @param n The size/window for the new bits
 * @param d The actually new bits to save in the defined position.
 * @pre n < sizeof(T) * 8
 * @pre s + n <= sizeof(T) * 8
 * @return The new value of \a x
 */
template <typename T, typename U>
constexpr T SB(T &x, const uint8_t s, const uint8_t n, const U d)
{
	x &= (T)(~((((T)1U << n) - 1) << s));
	typename std::make_unsigned<T>::type td = d;
	x |= (T)(td << s);
	return x;
}

/**
 * Add \a i to \a n bits of \a x starting at bit \a s.
 *
 * This adds the value of \a i on \a n bits of \a x starting at bit \a s. The parameters \a x,
 * \a s, \a i are similar to #GB. Besides, \ a x must be a variable as the result are
 * saved there. An overflow does not affect the following bits of the given
 * bit window and is simply ignored.
 *
 * @note Parameter x must be a variable as the result is saved there.
 * @param x The variable to add some bits at some position
 * @param s The start position of the addition
 * @param n The size/window for the addition
 * @pre n < sizeof(T) * 8
 * @pre s + n <= sizeof(T) * 8
 * @param i The value to add at the given start position in the given window.
 * @return The new value of \a x
 */
template <typename T, typename U>
constexpr T AB(T &x, const uint8_t s, const uint8_t n, const U i)
{
	const T mask = ((((T)1U << n) - 1) << s);
	x = (T)((x & ~mask) | ((x + (i << s)) & mask));
	return x;
}

/**
 * Checks if a bit in a value is set.
 *
 * This function checks if a bit inside a value is set or not.
 * The \a y value specific the position of the bit, started at the
 * LSB and count from \c 0.
 *
 * @param x The value to check
 * @param y The position of the bit to check, started from the LSB
 * @pre y < sizeof(T) * 8
 * @return True if the bit is set, false else.
 */
template <typename T>
debug_inline constexpr bool HasBit(const T x, const uint8_t y)
{
	return (x & ((T)1U << y)) != 0;
}

/**
 * Set a bit in a variable.
 *
 * This function sets a bit in a variable. The variable is changed
 * and the value is also returned. Parameter y defines the bit and
 * starts at the LSB with 0.
 *
 * @param x The variable to set a bit
 * @param y The bit position to set
 * @pre y < sizeof(T) * 8
 * @return The new value of the old value with the bit set
 */
template <typename T>
constexpr T SetBit(T &x, const uint8_t y)
{
	return x = (T)(x | ((T)1U << y));
}

/**
 * Sets several bits in a variable.
 *
 * This macro sets several bits in a variable. The bits to set are provided
 * by a value. The new value is also returned.
 *
 * @param x The variable to set some bits
 * @param y The value with set bits for setting them in the variable
 * @return The new value of x
 */
#define SETBITS(x, y) ((x) |= (y))

/**
 * Clears a bit in a variable.
 *
 * This function clears a bit in a variable. The variable is
 * changed and the value is also returned. Parameter y defines the bit
 * to clear and starts at the LSB with 0.
 *
 * @param x The variable to clear the bit
 * @param y The bit position to clear
 * @pre y < sizeof(T) * 8
 * @return The new value of the old value with the bit cleared
 */
template <typename T>
constexpr T ClrBit(T &x, const uint8_t y)
{
	return x = (T)(x & ~((T)1U << y));
}

/**
 * Clears several bits in a variable.
 *
 * This macro clears several bits in a variable. The bits to clear are
 * provided by a value. The new value is also returned.
 *
 * @param x The variable to clear some bits
 * @param y The value with set bits for clearing them in the variable
 * @return The new value of x
 */
#define CLRBITS(x, y) ((x) &= ~(y))

/**
 * Toggles a bit in a variable.
 *
 * This function toggles a bit in a variable. The variable is
 * changed and the value is also returned. Parameter y defines the bit
 * to toggle and starts at the LSB with 0.
 *
 * @param x The variable to toggle the bit
 * @param y The bit position to toggle
 * @pre y < sizeof(T) * 8
 * @return The new value of the old value with the bit toggled
 */
template <typename T>
constexpr T ToggleBit(T &x, const uint8_t y)
{
	return x = (T)(x ^ ((T)1U << y));
}

/**
 * Assigns a bit in a variable.
 *
 * This function assigns a single bit in a variable. The variable is
 * changed and the value is also returned. Parameter y defines the bit
 * to assign and starts at the LSB with 0.
 *
 * @param x The variable to assign the bit
 * @param y The bit position to assign
 * @param value The new bit value
 * @pre y < sizeof(T) * 8
 * @return The new value of the old value with the bit assigned
 */
template <typename T>
constexpr T AssignBit(T &x, const uint8_t y, bool value)
{
	return SB<T>(x, y, 1, value ? 1 : 0);
}

/**
 * Return a bit mask of count bits starting at start.
 *
 * @param start The start bit
 * @param count The number of bits
 * @return The bit mask
 */
template <typename T>
constexpr T GetBitMaskSC(const uint8_t start, const uint8_t count)
{
	using U = typename std::make_unsigned<T>::type;
	constexpr uint BIT_WIDTH = std::numeric_limits<U>::digits;

	U mask = ((static_cast<U>(1) << (count & (BIT_WIDTH - 1))) - 1) | (count >= BIT_WIDTH ? ~static_cast<U>(0) : 0);
	return (T)(mask << start);
}

/**
 * Return a bit mask of bits from first to last (inclusive).
 *
 * @param first The first bit
 * @param last The last bits (inclusive)
 * @pre first <= last && last < sizeof(T) * 8
 * @return The bit mask
 */
template <typename T>
constexpr T GetBitMaskFL(const uint8_t first, const uint8_t last)
{
	return GetBitMaskSC<T>(first, 1 + last - first);
}

/**
 * Return a bit mask of bits, set by bit number.
 *
 * @param bits The bits to set
 * @pre each bit < sizeof(T) * 8
 * @return The bit mask
 */
template <typename T, typename... Args>
constexpr T GetBitMaskBN(Args... bits)
{
	T value = static_cast<T>(0);
	(SetBit<T>(value, bits), ...);
	return value;
}

/**
 * Search the first set bit in a value.
 * When no bit is set, it returns 0.
 *
 * @param x The value to search.
 * @return The position of the first bit set.
 */
template <typename T>
constexpr uint8_t FindFirstBit(T x)
{
	if (x == 0) return 0;

	if constexpr (std::is_enum_v<T>) {
		return std::countr_zero<std::underlying_type_t<T>>(x);
	} else {
		return std::countr_zero(x);
	}
}

/**
 * Search the last set bit in a value.
 * When no bit is set, it returns 0.
 *
 * @param x The value to search.
 * @return The position of the last bit set.
 */
template <typename T>
constexpr uint8_t FindLastBit(T x)
{
	if (x == 0) return 0;

	return std::countl_zero<T>(1) - std::countl_zero<T>(x);
}

/**
 * Clear the first bit in an integer.
 *
 * This function returns a value where the first bit (from LSB)
 * is cleared.
 * So, 110100 returns 110000, 000001 returns 000000, etc.
 *
 * @param value The value to clear the first bit
 * @return The new value with the first bit cleared
 */
template <typename T>
constexpr T KillFirstBit(T value)
{
	return value &= (T)(value - 1);
}

/**
 * Counts the number of set bits in a variable.
 *
 * @param value the value to count the number of bits in.
 * @return the number of bits.
 */
template <typename T>
constexpr uint CountBits(T value)
{
	if constexpr (std::is_enum_v<T>) {
		return std::popcount<std::underlying_type_t<T>>(value);
	} else {
		return std::popcount(value);
	}
}

/**
 * Return whether the input has odd parity (odd number of bits set).
 *
 * @param value the value to return the parity of.
 * @return true if the parity is odd.
 */
template <typename T>
inline bool IsOddParity(T value)
{
	static_assert(sizeof(T) <= sizeof(unsigned long long));
	typename std::make_unsigned<T>::type unsigned_value = value;
#ifdef WITH_BITMATH_BUILTINS
	if (sizeof(T) <= sizeof(unsigned int)) {
		return __builtin_parity(unsigned_value);
	} else if (sizeof(T) == sizeof(unsigned long)) {
		return __builtin_parityl(unsigned_value);
	} else {
		return __builtin_parityll(unsigned_value);
	}
#else
	return CountBits(unsigned_value) & 1;
#endif
}

/**
 * Test whether \a value has exactly 1 bit set
 *
 * @param value the value to test.
 * @return does \a value have exactly 1 bit set?
 */
template <typename T>
constexpr bool HasExactlyOneBit(T value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

/**
 * Test whether \a value has at most 1 bit set
 *
 * @param value the value to test.
 * @return does \a value have at most 1 bit set?
 */
template <typename T>
constexpr bool HasAtMostOneBit(T value)
{
	return (value & (value - 1)) == 0;
}
template <int esize>
class Bitset
{
public:
	static const int bsize = esize / 64 + (esize % 64 ? 1 : 0);
	static const int msize = bsize * 8;

	uint64_t data[bsize];

	Bitset()
	{
		reset();
	}

	uint which_byte(uint n) const
	{
		for (uint b = 0; b < bsize; b++) {
			if (n < (b + 1) * 64) {
				return b;
			}
		}
		return (uint)-1;
	}

	void set(uint n, bool v)
	{
		uint b = which_byte(n);
		if (b >= bsize) {
			return;
		}
		int bit = n % 64;
		data[b] = (data[b] & ~((uint64_t)1 << bit)) | ((uint64_t)v << bit);
	}

	bool at(uint n) const
	{
		uint b= which_byte(n);
		if (b >= bsize) {
			return false;
		}
		return (data[b] & ((uint64_t)1 << (n % 64))) != 0;
	}

	bool all() const
	{
		uint b;
		for (b = 0; b < esize / 64; b++) {
			if (data[b] != (uint64_t)-1) {
				return false;
			}
		}
		if (esize % 64) {
			uint64_t bitmask = ~((uint64_t)-1 << (esize % 64));
			if ((data[b] & bitmask) != bitmask) {
				return false;
			}
		}
		return true;
	}

	bool none() const
	{
		uint b;
		for (b = 0; b < esize / 64; b++) {
			if (data[b] != (uint64_t)0) {
				return false;
			}
		}
		if (esize % 64) {
			uint64_t bitmask = ~((uint64_t)-1 << (esize % 64));
			if ((data[b] & bitmask) != 0) {
				return false;
			}
		}
		return true;
	}

	bool any() const
	{
		return !none();
	}

	uint count() const
	{
		if (all()) {
			return esize;
		}
		if (none()) {
			return 0;
		}
		uint c = 0, b;
		for (b = 0; b < esize / 64; b++) {
			for (uint i = 0; i < 64; i++) {
				if (data[b] & ((uint64_t)1 << i)) {
					c++;
				}
			}
		}
		uint mod = esize % 64;
		if (mod) {
			for (uint i = 0; i < mod; i++) {
				if (data[b] & ((uint64_t)1 << i)) {
					c++;
				}
			}
		}
		return c;
	}

	void toggle(uint n)
	{
		if (at(n)) {
			reset(n);
		} else {
			set(n);
		}
	}

	void reset()
	{
		memset(data, 0x00, msize);
	}

	void reset(uint n)
	{
		set(n, false);
	}

	void set()
	{
		memset(data, 0xFF, msize);
	}

	void set(uint n)
	{
		set(n, true);
	}

	bool compare(const Bitset<esize> &o) const
	{
		uint b;
		for (b = 0; b < esize / 64; b++) {
			if (data[b] != o.data[b]) {
				return false;
			}
		}
		if (esize % 64) {
			uint64_t bitmask = ~((uint64_t)-1 << (esize % 64));
			if ((data[b] & bitmask) != (o.data[b] & bitmask)) {
				return false;
			}
		}
		return true;
	}

	bool operator == (const Bitset<esize> &o) const
	{
		return compare(o);
	}

	bool operator != (const Bitset<esize> &o) const
	{
		return !compare(o);
	}
};

template <int esize>
static inline Bitset<esize> ClrBit(Bitset<esize> &x, const uint8_t y)
{
	x.set(y);
	return x;
}


 /**
 * Iterable ensemble of each set bit in a value.
 * @tparam Tbitpos Type of the position variable.
 * @tparam Tbitset Type of the bitset value.
 */
template <typename Tbitpos = uint, typename Tbitset = uint>
struct SetBitIterator {
	struct Iterator {
		typedef Tbitpos value_type;
		typedef value_type *pointer;
		typedef value_type &reference;
		typedef size_t difference_type;
		typedef std::forward_iterator_tag iterator_category;

		explicit Iterator(Tbitset bitset) : bitset(bitset), bitpos(static_cast<Tbitpos>(0))
		{
			this->Validate();
		}

		bool operator==(const Iterator &other) const
		{
			return this->bitset == other.bitset;
		}
		bool operator!=(const Iterator &other) const { return !(*this == other); }
		Tbitpos operator*() const { return this->bitpos; }
		Iterator & operator++() { this->Next(); this->Validate(); return *this; }

	private:
		Tbitset bitset;
		Tbitpos bitpos;
		void Validate()
		{
			if (this->bitset != 0) {
				typename std::make_unsigned<Tbitset>::type unsigned_value = this->bitset;
				this->bitpos = static_cast<Tbitpos>(FindFirstBit(unsigned_value));
			}
		}
		void Next()
		{
			this->bitset = KillFirstBit(this->bitset);
		}
	};

	SetBitIterator(Tbitset bitset) : bitset(bitset) {}

	Iterator begin() { return Iterator(this->bitset); }
	Iterator end() { return Iterator(static_cast<Tbitset>(0)); }
	bool empty() { return this->begin() == this->end(); }

private:
	Tbitset bitset;
};

namespace std {
	/**
	 * Custom implementation of std::byteswap; remove once we build with C++23.
	 * Perform an endianness bitswap on x.
	 * @param x the variable to bitswap
	 * @return the bitswapped value.
	 */
	template <typename T>
	[[nodiscard]] constexpr enable_if_t<is_integral_v<T>, T> byteswap(T x) noexcept
	{
		if constexpr (sizeof(T) == 1) return x;
#if !defined(__ICC) && (defined(__GNUC__) || defined(__clang__))
		if constexpr (sizeof(T) == 2) return static_cast<T>(__builtin_bswap16((uint16_t)x));
		if constexpr (sizeof(T) == 4) return static_cast<T>(__builtin_bswap32((uint32_t)x));
		if constexpr (sizeof(T) == 8) return static_cast<T>(__builtin_bswap64((uint64_t)x));
#else
		if constexpr (sizeof(T) == 2) return (x >> 8) | (x << 8);
		if constexpr (sizeof(T) == 4) return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
		if constexpr (sizeof(T) == 8) return ((x >> 56) & 0xFFULL) | ((x >> 40) & 0xFF00ULL) | ((x >> 24) & 0xFF0000ULL) | ((x >> 8) & 0xFF000000ULL) |
				((x << 8) & 0xFF00000000ULL) | ((x << 24) & 0xFF0000000000ULL) | ((x << 40) & 0xFF000000000000ULL) | ((x << 56) & 0xFF00000000000000ULL);
#endif
	}
}

#endif /* BITMATH_FUNC_HPP */
