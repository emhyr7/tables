#define USE_XXHASH 1

#include <immintrin.h>
#include <stdio.h>
#include <unordered_map>
#include <string_view>

#if USE_XXHASH
#define XXH_STATIC_LINKAGE_ONLY
#define XXH_IMPLEMENTATION
#define XXH_INLINE_ALL
#include "xxhash.h"
#endif

extern "C" int __stdcall QueryPerformanceCounter  (unsigned long long *);
extern "C" int __stdcall QueryPerformanceFrequency(unsigned long long *);

extern "C" void  __stdcall GetSystemInfo(void *);
extern "C" void *__stdcall VirtualAlloc (void *, unsigned long long, unsigned, unsigned);
extern "C" int   __stdcall VirtualFree  (void *, unsigned long long, unsigned);

typedef unsigned long long Address;
typedef Address            Size;
typedef signed long long   Index;
typedef signed long long   Count;

typedef long long Word;
typedef char      Byte;

typedef unsigned int       U32;
typedef unsigned long long U64;

typedef int                Boolean;

#define Assert(x, m) ({ if (!(x)) __builtin_debugtrap(); })

#define ALIGNOF(x) _Alignof(x)
#define WIDTHOF(x) (sizeof(x) * 8)
#define COUNTOF(x) (sizeof(x) / sizeof(*(x)))

inline Size QueryVirtualMemoryGranularity(void) {
	union {
		char _size[64];
		struct {
			_Alignas(4) char _pad[4];
			unsigned int pageSize;
		};
	} s;
	GetSystemInfo(&s);
	return s.pageSize;
}

inline void *AllocateVirtualMemory(Size size) {
	void *result = VirtualAlloc(0, size, 0x00001000 | 0x00002000, 0x04);
	Assert(result, "");
	return result;
}

inline void *ReserveVirtualMemory(Size size) {
	void *result = VirtualAlloc(0, size, 0x00002000, 0x04);
	Assert(result, "");
	return result;
}

inline void ReleaseVirtualMemory(void *address, Size size) {
	int result = VirtualFree(address, 0, 0x00008000);
	Assert(result, "");
}

inline void CommitVirtualMemory(void *address, Size size) {
	void *result = VirtualAlloc(address, size, 0x00001000, 0x04);
	Assert(result, "");
}

inline void DecommitVirtualMemory(void *address, Size size) {
	int result = VirtualFree(address, size, 0x00004000);
	Assert(result, "");
}

inline Size GaugeBackwardAligner(Size address, Size alignment) {
	return alignment ? address & (alignment - 1) : 0;
}

inline Size GaugeForwardAligner(Size address, Size alignment) {
	Size remainder = GaugeBackwardAligner(address, alignment);
	return remainder ? alignment - remainder : 0;
}

inline Size AlignBackwards(Size address, Size alignment) {
	return address - GaugeBackwardAligner(address, alignment);
}

inline Size AlignForwards(Size address, Size alignment) {
	return address + GaugeForwardAligner(address, alignment);
}

inline Boolean CheckAlignment(Size alignment) {
	return alignment && !(alignment & (alignment - 1));
}

inline U32 Hash_Murmur32(Byte *p, Count n) {
	U32 result = 0;
	for (Count i = 0; i < n; ++i) {
		Byte b = p[i];
		b *= 0xcc9e2d51;
		b = (b << 15 ) | (b >> (32 - 15) );
		b *= 0x1b873593;
		result ^= b;
		result = (result << 13 ) | (result >> (32 - 13));
		result = result * 5 + 0xe6546b64;
	}
	result ^= result >> 16;
	result *= 0x85ebca6b;
	result ^= result >> 13;
	result *= 0xc2b2ae35;
	result ^= result >> 16;
	return result;
}

inline U64 Hash_FNV1a(Byte *p, Count n) {
	U64 result = 14695981039346656037ULL;
	for (Count i = 0; i < n; ++i) {
		result = result ^ p[i];
		result = result * 1099511628211ULL;
	}
	return result;
}

#if USE_XXHASH
#define Hash(p, n) XXH3_64bits((Byte *)(p), n)
#else
#define Hash(p, n) Hash_FNV1a((Byte *)(p), n)
#endif

#define Size(s)       __builtin_strlen(s)
#define Test(a, b, n) __builtin_memcmp(a, b, n)
#define Fill(a, c, n) __builtin_memset(a, c, n)
#define Copy(a, b, n) __builtin_memcpy(a, b, n)
#define Rand()        ({U64 x; while (!_rdrand64_step(&x)); x;})

#define DEFAULT_EXTENT      (1ull << 32)
#define DEFAULT_QUANTITY    (1024ull << 3)
#define DEFAULT_GRANULARITY (128ull)

/*
a table that only increases in size. there're no capabilities to evict entries.

growth is the same as a dynamic array (perferrably use an arena-like).


*/

typedef struct {
	Size    extent;
	Address address;
	Size    quantity;
	Size    granularity;
} ITable;

void Initialize(ITable *table) {
	if (!table->extent     ) table->extent      = DEFAULT_EXTENT;
	if (!table->address    ) table->address     = (Address)AllocateVirtualMemory(table->extent);
	if (!table->quantity   ) table->quantity    = DEFAULT_QUANTITY;
	if (!table->granularity) table->granularity = DEFAULT_GRANULARITY;
	Fill((void *)table->address, 0, table->extent);
	Assert(CheckAlignment(table->quantity),);
	Assert(CheckAlignment(table->granularity),);
}

static Count Farts[DEFAULT_QUANTITY];

Index *Fetch(void *key, Count keysz, ITable *table) {
	Index *result = 0;

	Count linescnt = table->quantity;
	Count linesz = table->granularity;
	
	Index hash = Hash(key, keysz) & (linescnt - 1);
	Address addr = table->address + (hash << _tzcnt_u64(linesz));
	Address addrend = table->address + table->extent;
	Address lineaddr = addr;
	Count layersz = linescnt << _tzcnt_u64(linesz);

	Count remsz, remlinesz, inc;
	Byte *ptr = (Byte *)key;

	for (;;) {
		remsz = *(Count *)addr;
		addr += sizeof(Count);
		remlinesz = linesz - (addr - lineaddr);
		if (remsz == keysz) {
			for (;;) {
				inc = remsz <= remlinesz ? remsz : remlinesz;
				Boolean equals = !Test((Byte *)addr, ptr, inc);
				ptr += inc;
				if (equals) {
					remsz -= inc;
					if (!remsz) goto found;
				} else break;
				addr = lineaddr += layersz;
				remlinesz = linesz;
			}
			ptr = (Byte *)key;
		} else if (!remsz) goto enter;
		inc = remsz <= remlinesz ? remsz : remlinesz;
		remsz -= inc;
		if (remsz) {
			addr = lineaddr += ((remsz >> _tzcnt_u64(linesz)) + 1) << _tzcnt_u64(layersz);
			inc = remsz & (linesz - 1);
			remsz -= inc;
		}
		addr += inc;
		addr = AlignForwards(addr, ALIGNOF(Index));
		if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
		addr += sizeof(Index);
		if (addr > lineaddr + linesz - sizeof(Count)) addr = lineaddr += layersz;
		Assert(addr < addrend,);
	}
found:
	addr = AlignForwards(addr + inc, ALIGNOF(Index));
	if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
	result = (Index *)addr;
	return result;
enter:
	((Count *)addr)[-1] = keysz;
	remsz = keysz;
	for (;;) {
		inc = remsz <= remlinesz ? remsz : remlinesz;
		Copy((void *)addr, ptr, inc);
		ptr += inc;
		remsz -= inc;
		if (!remsz) break;
		addr = lineaddr += layersz;
		remlinesz = linesz;
	}
	addr = AlignForwards(addr + inc, ALIGNOF(Index));
	if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
	result = (Index *)addr;
	return result;
}



ITable table;

std::unordered_map<std::string_view, Index> cpptable;

Size ClockFrequency, ClockBeginning, ClockEnding;

#define BeginClock() (void)QueryPerformanceCounter(&ClockBeginning)
#define EndClock()   (void)QueryPerformanceCounter(&ClockEnding)
#define Elapse()     ((ClockEnding - ClockBeginning) * 1000000000 / ClockFrequency)

// #define KEY_SIZE   (64ull - sizeof(Index) - sizeof(Size))
#define KEY_SIZE   (32ull)
#define KEYS_COUNT (4096ull << 4)

#define ITERATIONS_COUNT (1024ull << 6)

int main(void) {
	(void)QueryPerformanceFrequency(&ClockFrequency);
	
	U64 rand = Rand();

	Size putavg, getavg, putmax, getmax;

	Byte chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	Count charscnt = COUNTOF(chars) - 1;

	Count keyscnt = KEYS_COUNT;
	Byte *keys = (Byte *)AllocateVirtualMemory(keyscnt * KEY_SIZE);
	Byte *key = keys;
	for (Count i = 0; i < keyscnt; ++i) {
		Count keysz = Rand() % (KEY_SIZE - 1) + 1;
		// Count keysz = KEY_SIZE;
		for (Count j = 0; j < keysz; ++j) key[j] = chars[Rand() % charscnt];
		key[keysz == KEY_SIZE ? KEY_SIZE - 1 : keysz] = 0;
		key += KEY_SIZE;
	}

	//for (Count i = 0; i < keyscnt; ++i) printf("[%lli] %s\n", i, keys + i * KEY_SIZE);
	
	printf("QUANTITY   : %llu\n", DEFAULT_QUANTITY);
	printf("GRANULARITY: %llu\n", DEFAULT_GRANULARITY);
	printf("KEY_SIZE   : %llu\n", KEY_SIZE);
	printf("KEYS_COUNT : %llu\n", KEYS_COUNT);
	printf("====================================\n");
	
	{
		std::string_view *cppkeys = (std::string_view *)AllocateVirtualMemory(sizeof(std::string_view) * keyscnt);
		for (Count i = 0; i < keyscnt; ++i) cppkeys[i] = std::string_view(keys + i * KEY_SIZE);

		putmax = 0, getmax = 0;
		{
			std::vector<Size> elapses;
			for (Count i = 0; i < keyscnt; ++i) {
				BeginClock();
				volatile Index *index = &cpptable[cppkeys[i]];
				*index = i;
				EndClock();
				Size elapse = Elapse();
				//printf("%llu\t\t%lli\t-> %lli\n", elapse, i, index);
				if (elapse > putmax) putmax = elapse;
				elapses.push_back(elapse);
			}
			Size avg = 0;
			for (auto i : elapses) avg += i;
			avg /= elapses.size();
			putavg = avg;
		}

		{
			std::vector<Size> elapses;
			for (Count i = 0; i < ITERATIONS_COUNT; ++i) {
				Count k = Rand() % keyscnt;
				BeginClock();
				volatile Index *index = &cpptable[cppkeys[k]];
				EndClock();
				Size elapse = Elapse();
				//printf("%llu\t\t%lli\t-> %lli\n", elapse, k, *index);
				if (elapse > getmax) getmax = elapse;
				elapses.push_back(elapse);
				// Assert(*index == k,);
			}
			Size avg = 0;
			for (auto i : elapses) avg += i;
			avg /= elapses.size();
			getavg = avg;
		}
	}

	printf("cpptable:\n");
	printf("\tput: %llu,\t%llu\n\tget: %llu,\t%llu\n", putavg, putmax, getavg, getmax);
	printf("====================================\n");

	{
		putmax = 0, getmax = 0;
		{
			std::vector<Size> elapses;
			Initialize(&table);
			for (Count i = 0; i < keyscnt; ++i) {
				key = keys + i * KEY_SIZE;
				Count keysz = Size(key);
				BeginClock();
				volatile Index *index = Fetch(key, keysz, &table);
				*index = i;
				EndClock();
				Size elapse = Elapse();
				//printf("%llu\t\t%lli\t-> %lli\n", elapse, i, *index);
				if (elapse > putmax) putmax = elapse;
				elapses.push_back(elapse);
			}
			Size avg = 0;
			for (auto i : elapses) avg += i;
			avg /= elapses.size();
			putavg = avg;
		}

		{
			std::vector<Size> elapses;
			for (Count i = 0; i < ITERATIONS_COUNT; ++i) {
				Count k = Rand() % keyscnt;
				key = keys + k * KEY_SIZE;
				Count keysz = Size(key);
				BeginClock();
				volatile Index *index = Fetch(key, keysz, &table);
				EndClock();
				Size elapse = Elapse();
				//printf("%llu\t\t%lli\t-> %lli\n", elapse, k, *index);
				if (elapse > getmax) getmax = elapse;
				elapses.push_back(elapse);
				//Assert(*index == k,);
			}
			Size avg = 0;
			for (auto i : elapses) avg += i;
			avg /= elapses.size();
			getavg = avg;
		}
	}

	printf("itable:\n");
	printf("\tput: %llu,\t%llu\n\tget: %llu,\t%llu\n", putavg, putmax, getavg, getmax);
	printf("====================================\n");
	
	Size fartsavg = 0, fartsmax = 0;
	for (Count i = 0; i < COUNTOF(Farts); ++i) {
		fartsavg += Farts[i];
		if (Farts[i] > fartsmax) fartsmax = Farts[i];
		//printf("[%lli] %lli\n", i, Farts[i]);
	}
	fartsavg /= COUNTOF(Farts);
	printf("farts: %llu,\t%llu\n", fartsavg, fartsmax);

	printf("cpptable.max_bucket_count: %llu\ncpptable.bucket_count: %llu\n", cpptable.max_bucket_count(), cpptable.bucket_count());

	//getchar();
	return 0;
}
