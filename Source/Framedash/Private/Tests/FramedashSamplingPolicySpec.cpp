// Copyright 2026 Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

// Dev-only AND opt-in, identical gate to the sibling specs: even under
// WITH_DEV_AUTOMATION_TESTS, BuildPlugin emits Development/Editor DLLs, so gating on that
// alone would ship spec code inside the redistributable binaries.
// FRAMEDASH_WITH_AUTOMATION_SPECS is defined to 1 only when Framedash.Build.cs sees
// FRAMEDASH_BUILD_AUTOMATION_SPECS=1 (set by the automation-spec CI job and the README
// local-run instructions).
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"

#include "FramedashSamplingPolicy.h"

// In-engine coverage for the FFramedashSamplingPolicy sampling contract, ported from
// sdks/unity/Tests/SamplingPolicyTests.cs so the three SDKs pin the same semantics. The
// engine-independent GoogleTest harness (sdks/ue5/Tests) cannot reach this class: its
// public API takes FString and it holds a TMap behind an FCriticalSection, and the
// settled guardrail forbids extracting UE-type-coupled classes to pure C++.
//
// Only deterministic behavior is asserted. ShouldSample falls through to FMath::FRand()
// for a rate strictly between 0 and 1, so the Unity suite's ~50%-ratio statistical check
// is deliberately not ported -- a flaky in-engine spec would block every UE5 PR. The
// stored partial rate is asserted instead, which is what the clamping contract owns.

namespace
{
	// Repeat count for the always/never assertions. The rate-0 and rate-1 paths short
	// circuit before the RNG, so this only guards against a future change that starts
	// drawing on those paths -- it is not a statistical sample.
	constexpr int32 DeterministicTrials = 100;

	bool SamplesEveryTime(const FFramedashSamplingPolicy& Policy, const FString& EventName)
	{
		for (int32 Index = 0; Index < DeterministicTrials; ++Index)
		{
			if (!Policy.ShouldSample(EventName))
			{
				return false;
			}
		}
		return true;
	}

	bool SamplesNever(const FFramedashSamplingPolicy& Policy, const FString& EventName)
	{
		for (int32 Index = 0; Index < DeterministicTrials; ++Index)
		{
			if (Policy.ShouldSample(EventName))
			{
				return false;
			}
		}
		return true;
	}
}

BEGIN_DEFINE_SPEC(FFramedashSamplingPolicySpec, "Framedash.Sampling.Policy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FFramedashSamplingPolicySpec)

void FFramedashSamplingPolicySpec::Define()
{
	Describe("the global rate", [this]()
	{
		It("always samples at rate 1", [this]()
		{
			const FFramedashSamplingPolicy Policy(1.0f);
			TestTrue(TEXT("rate 1 samples every event"), SamplesEveryTime(Policy, TEXT("evt")));
		});

		It("never samples at rate 0", [this]()
		{
			const FFramedashSamplingPolicy Policy(0.0f);
			TestTrue(TEXT("rate 0 drops every event"), SamplesNever(Policy, TEXT("evt")));
		});

		It("clamps the constructor value to the unit interval", [this]()
		{
			TestEqual(TEXT("above-range rate clamps to 1"), FFramedashSamplingPolicy(2.0f).GetRate(), 1.0f);
			TestEqual(TEXT("below-range rate clamps to 0"), FFramedashSamplingPolicy(-1.0f).GetRate(), 0.0f);
		});

		It("clamps the setter value to the unit interval", [this]()
		{
			FFramedashSamplingPolicy Policy(0.5f);
			TestEqual(TEXT("in-range constructor rate is preserved"), Policy.GetRate(), 0.5f);

			Policy.SetRate(1.5f);
			TestEqual(TEXT("above-range rate clamps to 1"), Policy.GetRate(), 1.0f);

			Policy.SetRate(-0.5f);
			TestEqual(TEXT("below-range rate clamps to 0"), Policy.GetRate(), 0.0f);

			Policy.SetRate(0.75f);
			TestEqual(TEXT("in-range rate is preserved"), Policy.GetRate(), 0.75f);
		});
	});

	Describe("per-event overrides", [this]()
	{
		It("keeps an event the global rate would drop", [this]()
		{
			FFramedashSamplingPolicy Policy(0.0f);
			Policy.SetEventRate(TEXT("keep_me"), 1.0f);

			TestTrue(TEXT("overridden event is always sampled"), SamplesEveryTime(Policy, TEXT("keep_me")));
			TestTrue(TEXT("other events still follow the global rate"), SamplesNever(Policy, TEXT("other")));
		});

		It("drops an event the global rate would keep", [this]()
		{
			FFramedashSamplingPolicy Policy(1.0f);
			Policy.SetEventRate(TEXT("drop_me"), 0.0f);

			TestTrue(TEXT("overridden event is never sampled"), SamplesNever(Policy, TEXT("drop_me")));
			TestTrue(TEXT("other events still follow the global rate"), SamplesEveryTime(Policy, TEXT("other")));
		});

		It("clamps the override rate to the unit interval", [this]()
		{
			FFramedashSamplingPolicy Policy(0.5f);

			Policy.SetEventRate(TEXT("evt"), 2.0f);
			TestTrue(TEXT("above-range override clamps to 1"), SamplesEveryTime(Policy, TEXT("evt")));

			Policy.SetEventRate(TEXT("evt"), -1.0f);
			TestTrue(TEXT("below-range override clamps to 0"), SamplesNever(Policy, TEXT("evt")));
		});

		It("restores the global rate when the override is removed", [this]()
		{
			FFramedashSamplingPolicy Policy(1.0f);
			Policy.SetEventRate(TEXT("evt"), 0.0f);
			TestFalse(TEXT("override suppresses the event"), Policy.ShouldSample(TEXT("evt")));

			TestTrue(TEXT("removing a present override reports true"), Policy.RemoveEventRate(TEXT("evt")));
			TestTrue(TEXT("global rate applies again"), SamplesEveryTime(Policy, TEXT("evt")));

			TestFalse(TEXT("removing an absent override reports false"), Policy.RemoveEventRate(TEXT("evt")));
		});

		It("ignores an empty event name", [this]()
		{
			FFramedashSamplingPolicy Policy(1.0f);
			Policy.SetEventRate(FString(), 0.0f);

			TestTrue(TEXT("no override was registered"), SamplesEveryTime(Policy, TEXT("anything")));
			TestFalse(TEXT("removing an empty name reports false"), Policy.RemoveEventRate(FString()));
		});
	});

	Describe("an empty event name", [this]()
	{
		It("falls back to the global rate", [this]()
		{
			const FFramedashSamplingPolicy Keep(1.0f);
			TestTrue(TEXT("empty name follows a keep-all global rate"), Keep.ShouldSample(FString()));

			const FFramedashSamplingPolicy Drop(0.0f);
			TestFalse(TEXT("empty name follows a drop-all global rate"), Drop.ShouldSample(FString()));
		});
	});
}

#endif
