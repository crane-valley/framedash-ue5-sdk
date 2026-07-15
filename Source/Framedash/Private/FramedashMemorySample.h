// Copyright Crane Valley. All Rights Reserved.
//
// Engine-independent selection rules for the mem.* memory-detail metrics
// attached to perf_heartbeat events (metrics map / proto field 13). The RHI /
// LLM reads themselves are engine-coupled and live in
// FramedashPerformanceCollector; this header holds ONLY the pure "which keys are
// present, and with what value" decision so it can be unit-tested in the
// GoogleTest harness (sdks/ue5/Tests) without an UnrealEditor build -- same
// contract as FramedashIoStats.h / FramedashFieldClamps.h. It includes NO
// Unreal headers on purpose.
//
// Absent = not collected: the core never fabricates a 0 for an unavailable
// value. mem.vram is emitted whenever the RHI produced a (non-negative) reading
// -- a real 0 (nothing allocated yet) is a collected value and IS emitted, while
// an unavailable RHI (headless / nullrhi / early startup) leaves the key absent.
// The three LLM categories are emitted only when LLM was enabled at runtime AND
// the tag reports a positive tracked amount; a 0 is indistinguishable from
// "untracked", so it stays absent.

#pragma once

#include <cstddef>
#include <cstdint>

// Canonical mem.* metric key names. Defined as macros so the engine-independent
// constants below (used by the GoogleTest harness) and the FString keys the
// subsystem emits via TEXT(...) resolve to the SAME literal -- a key rename can
// never drift between the tested contract and the emitted wire key.
#define FRAMEDASH_MEM_KEY_VRAM "mem.vram"
#define FRAMEDASH_MEM_KEY_TEXTURES "mem.textures"
#define FRAMEDASH_MEM_KEY_MESHES "mem.meshes"
#define FRAMEDASH_MEM_KEY_AUDIO "mem.audio"

namespace Framedash
{
	inline constexpr const char* kMemKeyVram = FRAMEDASH_MEM_KEY_VRAM;
	inline constexpr const char* kMemKeyTextures = FRAMEDASH_MEM_KEY_TEXTURES;
	inline constexpr const char* kMemKeyMeshes = FRAMEDASH_MEM_KEY_MESHES;
	inline constexpr const char* kMemKeyAudio = FRAMEDASH_MEM_KEY_AUDIO;

	// Raw sampled inputs, filled by the engine-coupled sampler. The bHas* flags
	// distinguish "the sampler ran and produced this value" from "the source was
	// unavailable", so the select step can honor absent-vs-present without ever
	// inventing a 0.
	struct FMemorySampleInputs
	{
		// True when the RHI was available and RHIGetTextureMemoryStats was read.
		bool bRhiValid = false;
		// Video memory in use (bytes): streaming + non-streaming texture allocations.
		int64_t VramBytes = 0;

		// True when LLM was compiled in AND enabled at runtime (-llm). When false,
		// the category amounts below are meaningless and never emitted.
		bool bLlmEnabled = false;
		int64_t TexturesBytes = 0;
		int64_t MeshesBytes = 0;
		int64_t AudioBytes = 0;
	};

	// Per-key presence + value after applying the emit rules. Only fields with the
	// matching bHas* set are attached to the metrics map by the caller.
	struct FMemoryMetrics
	{
		bool bHasVram = false;
		int64_t Vram = 0;
		bool bHasTextures = false;
		int64_t Textures = 0;
		bool bHasMeshes = false;
		int64_t Meshes = 0;
		bool bHasAudio = false;
		int64_t Audio = 0;
	};

	inline FMemoryMetrics SelectMemoryMetrics(const FMemorySampleInputs& In)
	{
		FMemoryMetrics Out;

		// mem.vram: present whenever the RHI produced a non-negative reading. A
		// valid RHI reporting 0 bytes in use (nothing allocated) is a collected 0
		// and IS emitted -- distinct from an unavailable RHI, which leaves bRhiValid
		// false and the key absent. The >= 0 guard is purely defensive against an
		// int64 overflow of the uint64 texture-stat sum.
		if (In.bRhiValid && In.VramBytes >= 0)
		{
			Out.bHasVram = true;
			Out.Vram = In.VramBytes;
		}

		// mem.textures / mem.meshes / mem.audio: only when LLM is enabled AND the
		// tag reports a positive tracked amount. A non-positive amount means the
		// category is untracked/empty and is indistinguishable from "not populated
		// yet", so it stays absent rather than emitting a misleading 0.
		if (In.bLlmEnabled)
		{
			if (In.TexturesBytes > 0)
			{
				Out.bHasTextures = true;
				Out.Textures = In.TexturesBytes;
			}
			if (In.MeshesBytes > 0)
			{
				Out.bHasMeshes = true;
				Out.Meshes = In.MeshesBytes;
			}
			if (In.AudioBytes > 0)
			{
				Out.bHasAudio = true;
				Out.Audio = In.AudioBytes;
			}
		}

		return Out;
	}

	// Presence bits for the mem.* keys, in canonical order. Used to diff the key set
	// between consecutive heartbeats so the persistent HeartbeatMetrics map is only
	// mutated on a transition (steady state stays zero heap allocation).
	enum EMemoryKeyBit : uint32_t
	{
		kMemBitVram = 1u << 0,
		kMemBitTextures = 1u << 1,
		kMemBitMeshes = 1u << 2,
		kMemBitAudio = 1u << 3,
	};

	// Bitmask of the categories present in a selected sample.
	inline uint32_t MemoryPresenceMask(const FMemoryMetrics& M)
	{
		uint32_t Mask = 0;
		if (M.bHasVram)
		{
			Mask |= kMemBitVram;
		}
		if (M.bHasTextures)
		{
			Mask |= kMemBitTextures;
		}
		if (M.bHasMeshes)
		{
			Mask |= kMemBitMeshes;
		}
		if (M.bHasAudio)
		{
			Mask |= kMemBitAudio;
		}
		return Mask;
	}

	// Keys present in the PREVIOUS heartbeat but absent now -> must be Removed from the
	// persistent map. Keys still present are updated in place (no alloc); genuinely new
	// keys are Added. This is the pure form of the heartbeat's steady-state-zero-alloc
	// diff: an empty result means nothing changed, so the map is not mutated.
	inline uint32_t StaleMemoryKeysMask(uint32_t PrevMask, uint32_t CurrMask)
	{
		return PrevMask & ~CurrMask;
	}

	// One present mem.* key/value to attach. Key points at a kMemKey* literal (static
	// storage), so it is safe to hold without copying.
	struct FMemoryMetricEntry
	{
		const char* Key = nullptr;
		int64_t Value = 0;
	};

	// Maximum entries CollectMemoryMetricEntries can write (vram + 3 categories).
	inline constexpr int32_t kMaxMemoryMetricEntries = 4;

	// Enumerate the PRESENT keys of a selected sample into Out (capacity
	// kMaxMemoryMetricEntries) and return how many were written. Absent keys are
	// skipped, so "absent stays absent" is enforced in one place shared by the
	// heartbeat and event-attach paths. This is the pure, host-testable core of the
	// "which mem.* keys does this sample emit" decision.
	inline int32_t CollectMemoryMetricEntries(const FMemoryMetrics& M, FMemoryMetricEntry* Out)
	{
		int32_t Count = 0;
		if (M.bHasVram)
		{
			Out[Count++] = FMemoryMetricEntry{kMemKeyVram, M.Vram};
		}
		if (M.bHasTextures)
		{
			Out[Count++] = FMemoryMetricEntry{kMemKeyTextures, M.Textures};
		}
		if (M.bHasMeshes)
		{
			Out[Count++] = FMemoryMetricEntry{kMemKeyMeshes, M.Meshes};
		}
		if (M.bHasAudio)
		{
			Out[Count++] = FMemoryMetricEntry{kMemKeyAudio, M.Audio};
		}
		return Count;
	}

	// Decide which enumerated mem.* entries a position-qualified event may attach,
	// given the caller-metrics merge that already ran. This is the pure form of the
	// subsystem's caller-wins / caller-first-capacity rule (Codex round 2):
	//   - CallerHasKey[i] true  -> the caller already set entry i, so it is SKIPPED
	//     (the caller's value wins; the SDK never overwrites it).
	//   - entries fill only RemainingCapacity slots (= MaxMetricPairs minus the count
	//     the caller loop already consumed). A caller that filled the cap leaves
	//     RemainingCapacity 0, so NO mem.* attach and NO caller metric is displaced --
	//     identical to pre-feature behavior.
	// Order is preserved; chosen entries are written to Out (capacity >= NumEntries)
	// and the count is returned. Skipped (caller-set) entries do NOT consume capacity.
	inline int32_t SelectAttachableMemoryEntries(
		const FMemoryMetricEntry* Entries,
		int32_t NumEntries,
		const bool* CallerHasKey,
		int32_t RemainingCapacity,
		FMemoryMetricEntry* Out)
	{
		int32_t Count = 0;
		for (int32_t Index = 0; Index < NumEntries; ++Index)
		{
			if (Count >= RemainingCapacity)
			{
				break;
			}
			if (CallerHasKey != nullptr && CallerHasKey[Index])
			{
				continue;
			}
			Out[Count++] = Entries[Index];
		}
		return Count;
	}
} // namespace Framedash
