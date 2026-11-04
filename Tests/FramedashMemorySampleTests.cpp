// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Engine-independent coverage for the mem.* key-selection rules
// (SelectMemoryMetrics). The RHI/LLM reads themselves are engine-coupled and not
// host-testable; these tests pin the absent-vs-present contract that decides
// which keys ride perf_heartbeat.

#include "FramedashMemorySample.h"

#include <cstring>

#include <gtest/gtest.h>

using Framedash::CollectMemoryMetricEntries;
using Framedash::FMemoryMetricEntry;
using Framedash::FMemoryMetrics;
using Framedash::FMemorySampleInputs;
using Framedash::kMaxMemoryMetricEntries;
using Framedash::MemoryPresenceMask;
using Framedash::SelectAttachableMemoryEntries;
using Framedash::SelectMemoryMetrics;
using Framedash::StaleMemoryKeysMask;

TEST(MemorySampleTest, NoSourceEmitsNothing)
{
	FMemorySampleInputs In;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_FALSE(Out.bHasVram);
	EXPECT_FALSE(Out.bHasTextures);
	EXPECT_FALSE(Out.bHasMeshes);
	EXPECT_FALSE(Out.bHasAudio);
}

TEST(MemorySampleTest, ValidRhiEmitsVramIncludingCollectedZero)
{
	FMemorySampleInputs In;
	In.bRhiValid = true;
	In.VramBytes = 0;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_TRUE(Out.bHasVram);
	EXPECT_EQ(Out.Vram, 0);
}

TEST(MemorySampleTest, ValidRhiEmitsPositiveVram)
{
	FMemorySampleInputs In;
	In.bRhiValid = true;
	In.VramBytes = 512 * 1024 * 1024;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_TRUE(Out.bHasVram);
	EXPECT_EQ(Out.Vram, 512 * 1024 * 1024);
}

TEST(MemorySampleTest, UnavailableRhiLeavesVramAbsentEvenWithValue)
{
	// bRhiValid gates emission: a leftover VramBytes with no valid RHI stays absent
	// (never emit a fabricated value for an unavailable source).
	FMemorySampleInputs In;
	In.bRhiValid = false;
	In.VramBytes = 999;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_FALSE(Out.bHasVram);
}

TEST(MemorySampleTest, NegativeVramDefensivelyDropped)
{
	FMemorySampleInputs In;
	In.bRhiValid = true;
	In.VramBytes = -1;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_FALSE(Out.bHasVram);
}

TEST(MemorySampleTest, LlmDisabledEmitsNoCategoriesEvenWithAmounts)
{
	// bLlmEnabled=false means the amounts are meaningless: no category key emitted.
	FMemorySampleInputs In;
	In.bLlmEnabled = false;
	In.TexturesBytes = 100;
	In.MeshesBytes = 200;
	In.AudioBytes = 300;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_FALSE(Out.bHasTextures);
	EXPECT_FALSE(Out.bHasMeshes);
	EXPECT_FALSE(Out.bHasAudio);
}

TEST(MemorySampleTest, LlmEnabledEmitsOnlyPositiveCategories)
{
	FMemorySampleInputs In;
	In.bLlmEnabled = true;
	In.TexturesBytes = 4096;
	In.MeshesBytes = 0;
	In.AudioBytes = 128;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_TRUE(Out.bHasTextures);
	EXPECT_EQ(Out.Textures, 4096);
	EXPECT_FALSE(Out.bHasMeshes);
	EXPECT_TRUE(Out.bHasAudio);
	EXPECT_EQ(Out.Audio, 128);
}

TEST(MemorySampleTest, VramAndCategoriesAreIndependent)
{
	// mem.vram (RHI) is emitted without LLM; categories require LLM. Both sources
	// can be present at once and do not gate each other.
	FMemorySampleInputs In;
	In.bRhiValid = true;
	In.VramBytes = 1024;
	In.bLlmEnabled = true;
	In.TexturesBytes = 2048;
	const FMemoryMetrics Out = SelectMemoryMetrics(In);
	EXPECT_TRUE(Out.bHasVram);
	EXPECT_EQ(Out.Vram, 1024);
	EXPECT_TRUE(Out.bHasTextures);
	EXPECT_EQ(Out.Textures, 2048);
	EXPECT_FALSE(Out.bHasMeshes);
	EXPECT_FALSE(Out.bHasAudio);
}


TEST(MemoryMetricEntriesTest, EmptySampleEmitsNoEntries)
{
	FMemoryMetrics M;
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	EXPECT_EQ(CollectMemoryMetricEntries(M, Entries), 0);
}

TEST(MemoryMetricEntriesTest, EnumeratesOnlyPresentKeysWithCanonicalNames)
{
	FMemoryMetrics M;
	M.bHasVram = true;
	M.Vram = 4096;
	M.bHasAudio = true;
	M.Audio = 64;
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	const int Count = CollectMemoryMetricEntries(M, Entries);
	ASSERT_EQ(Count, 2);
	EXPECT_STREQ(Entries[0].Key, "mem.vram");
	EXPECT_EQ(Entries[0].Value, 4096);
	EXPECT_STREQ(Entries[1].Key, "mem.audio");
	EXPECT_EQ(Entries[1].Value, 64);
}

TEST(MemoryMetricEntriesTest, AllPresentEnumeratesFourCanonicalKeys)
{
	FMemoryMetrics M;
	M.bHasVram = true;
	M.Vram = 1;
	M.bHasTextures = true;
	M.Textures = 2;
	M.bHasMeshes = true;
	M.Meshes = 3;
	M.bHasAudio = true;
	M.Audio = 4;
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	const int Count = CollectMemoryMetricEntries(M, Entries);
	ASSERT_EQ(Count, kMaxMemoryMetricEntries);
	EXPECT_STREQ(Entries[0].Key, "mem.vram");
	EXPECT_STREQ(Entries[1].Key, "mem.textures");
	EXPECT_STREQ(Entries[2].Key, "mem.meshes");
	EXPECT_STREQ(Entries[3].Key, "mem.audio");
}

TEST(MemoryMetricEntriesTest, KeyMacrosMatchConstants)
{
	// The FRAMEDASH_MEM_KEY_* macros (used by the subsystem via TEXT(...)) and the
	// kMemKey* constants (used here) must resolve to identical literals so the emitted
	// wire keys cannot drift from the tested contract.
	EXPECT_STREQ(Framedash::kMemKeyVram, FRAMEDASH_MEM_KEY_VRAM);
	EXPECT_STREQ(Framedash::kMemKeyTextures, FRAMEDASH_MEM_KEY_TEXTURES);
	EXPECT_STREQ(Framedash::kMemKeyMeshes, FRAMEDASH_MEM_KEY_MESHES);
	EXPECT_STREQ(Framedash::kMemKeyAudio, FRAMEDASH_MEM_KEY_AUDIO);
}


namespace
{
	void MakeFourEntries(FMemoryMetricEntry (&Entries)[kMaxMemoryMetricEntries])
	{
		Entries[0] = FMemoryMetricEntry{"mem.vram", 1};
		Entries[1] = FMemoryMetricEntry{"mem.textures", 2};
		Entries[2] = FMemoryMetricEntry{"mem.meshes", 3};
		Entries[3] = FMemoryMetricEntry{"mem.audio", 4};
	}
}

TEST(MemoryAttachTest, AmpleCapacityNoCallerKeysAttachesAll)
{
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	MakeFourEntries(Entries);
	const bool CallerHasKey[kMaxMemoryMetricEntries] = {false, false, false, false};
	FMemoryMetricEntry Out[kMaxMemoryMetricEntries];
	const int Count = SelectAttachableMemoryEntries(Entries, 4, CallerHasKey,  8, Out);
	ASSERT_EQ(Count, 4);
	EXPECT_STREQ(Out[0].Key, "mem.vram");
	EXPECT_STREQ(Out[3].Key, "mem.audio");
}

TEST(MemoryAttachTest, ZeroRemainingCapacityAttachesNothing)
{
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	MakeFourEntries(Entries);
	const bool CallerHasKey[kMaxMemoryMetricEntries] = {false, false, false, false};
	FMemoryMetricEntry Out[kMaxMemoryMetricEntries];
	const int Count = SelectAttachableMemoryEntries(Entries, 4, CallerHasKey,  0, Out);
	EXPECT_EQ(Count, 0);
}

TEST(MemoryAttachTest, CallerSetKeysAreSkippedWithoutConsumingCapacity)
{
	// Caller already set vram and meshes -> those are skipped (caller wins). The two
	// skipped entries do NOT consume the remaining capacity, so both textures and
	// audio still attach even with RemainingCapacity == 2.
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	MakeFourEntries(Entries);
	const bool CallerHasKey[kMaxMemoryMetricEntries] = {true, false, true, false};
	FMemoryMetricEntry Out[kMaxMemoryMetricEntries];
	const int Count = SelectAttachableMemoryEntries(Entries, 4, CallerHasKey,  2, Out);
	ASSERT_EQ(Count, 2);
	EXPECT_STREQ(Out[0].Key, "mem.textures");
	EXPECT_STREQ(Out[1].Key, "mem.audio");
}

TEST(MemoryAttachTest, RemainingCapacityLimitsAttachInOrder)
{
	FMemoryMetricEntry Entries[kMaxMemoryMetricEntries];
	MakeFourEntries(Entries);
	const bool CallerHasKey[kMaxMemoryMetricEntries] = {false, false, false, false};
	FMemoryMetricEntry Out[kMaxMemoryMetricEntries];
	const int Count = SelectAttachableMemoryEntries(Entries, 4, CallerHasKey,  1, Out);
	ASSERT_EQ(Count, 1);
	EXPECT_STREQ(Out[0].Key, "mem.vram");
}


TEST(MemoryMaskTest, PresenceMaskReflectsPresentCategories)
{
	FMemoryMetrics M;
	EXPECT_EQ(MemoryPresenceMask(M), 0u);
	M.bHasVram = true;
	M.bHasMeshes = true;
	EXPECT_EQ(MemoryPresenceMask(M), Framedash::kMemBitVram | Framedash::kMemBitMeshes);
	M.bHasTextures = true;
	M.bHasAudio = true;
	EXPECT_EQ(MemoryPresenceMask(M),
		Framedash::kMemBitVram | Framedash::kMemBitTextures |
		Framedash::kMemBitMeshes | Framedash::kMemBitAudio);
}

TEST(MemoryMaskTest, UnchangedKeySetHasNoStaleKeys)
{
	// Steady state: same key set as last heartbeat -> nothing to remove (so the map is
	// not mutated -> zero allocation).
	const uint32_t Mask = Framedash::kMemBitVram | Framedash::kMemBitTextures;
	EXPECT_EQ(StaleMemoryKeysMask(Mask, Mask), 0u);
}

TEST(MemoryMaskTest, StaleKeysAreThosePresentBeforeButGoneNow)
{
	const uint32_t Prev = Framedash::kMemBitVram | Framedash::kMemBitTextures | Framedash::kMemBitMeshes;
	const uint32_t Curr = Framedash::kMemBitVram;
	EXPECT_EQ(StaleMemoryKeysMask(Prev, Curr), Framedash::kMemBitTextures | Framedash::kMemBitMeshes);
}

TEST(MemoryMaskTest, NewlyPresentKeysAreNotStale)
{
	const uint32_t Prev = Framedash::kMemBitVram;
	const uint32_t Curr = Framedash::kMemBitVram | Framedash::kMemBitAudio;
	EXPECT_EQ(StaleMemoryKeysMask(Prev, Curr), 0u);
}
