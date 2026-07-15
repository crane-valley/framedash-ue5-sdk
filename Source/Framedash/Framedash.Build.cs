// Copyright Crane Valley. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

public class Framedash : ModuleRules
{
	public Framedash(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// nanopb uses C99 features that can trip the compiler's "undefined
		// identifier" preprocessor warning (C4668 on MSVC, -Wundef on Clang/GCC).
		// The API to silence it moved across engine releases, so pick the
		// version-appropriate one to keep the plugin buildable on UE 5.3 through
		// the current release without deprecation warnings:
		//   UE 5.6+  : CppCompileWarningSettings.UndefinedIdentifierWarningLevel
		//   UE 5.5   : ModuleRules.UndefinedIdentifierWarningLevel
		//   UE 5.3-4 : bEnableUndefinedIdentifierWarnings
#if UE_5_6_OR_LATER
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
#elif UE_5_5_OR_LATER
		UndefinedIdentifierWarningLevel = WarningLevel.Off;
#else
		bEnableUndefinedIdentifierWarnings = false;
#endif

		string ThirdPartyPath = Path.Combine(ModuleDirectory, "Private", "ThirdParty");
		string ProtoPath = Path.Combine(ModuleDirectory, "Private", "Proto");

		PublicIncludePaths.AddRange(new string[] { });

		PrivateIncludePaths.AddRange(new string[] {
			Path.Combine(ThirdPartyPath, "nanopb"),
			ProtoPath,
		});

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json",
			"RenderCore",
			"RHI",
		});

		// Direct-socket TLS fallback for the prefer-IPv4-with-IPv6-fallback
		// ingest connect (FramedashDirectSocketSender). Compiled only where
		// the engine SSL module actually provides OpenSSL -- the platform
		// predicate mirrors SSL.Build.cs's own WITH_SSL gate -- so platforms
		// without it keep the existing IHttpRequest-only transport untouched.
		bool bSupportsDirectSocketTls =
			Target.Platform == UnrealTargetPlatform.Win64 ||
			Target.Platform == UnrealTargetPlatform.Mac ||
			Target.Platform == UnrealTargetPlatform.IOS ||
			Target.Platform == UnrealTargetPlatform.Android ||
			Target.IsInPlatformGroup(UnrealPlatformGroup.Unix);
		if (bSupportsDirectSocketTls)
		{
			PrivateDependencyModuleNames.AddRange(new string[] {
				"SSL",
				"Sockets",
			});
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
			PrivateDefinitions.Add("FRAMEDASH_WITH_DIRECT_SOCKET_TLS=1");
		}
		else
		{
			PrivateDefinitions.Add("FRAMEDASH_WITH_DIRECT_SOCKET_TLS=0");
		}

		// nanopb C files need special handling
		PublicDefinitions.Add("PB_FIELD_32BIT=1");

		// Opt-in gate for the in-engine Automation Specs (Private/Tests/*Spec.cpp). Even
		// under WITH_DEV_AUTOMATION_TESTS, BuildPlugin emits Development/Editor DLLs, so
		// without this gate the specs -- whose setup destructively clears the project's
		// offline queue -- would ship inside the redistributable binaries. Defined to 1
		// ONLY when FRAMEDASH_BUILD_AUTOMATION_SPECS=1 is set in the environment (the
		// automation-spec CI job and the README local-run steps set it), so BuildPlugin
		// and normal game builds never compile the specs. Trade-off: the BuildPlugin
		// matrix no longer compile-checks the spec -- acceptable, the automation-spec job
		// builds and runs it every run.
		bool bBuildAutomationSpecs =
			Environment.GetEnvironmentVariable("FRAMEDASH_BUILD_AUTOMATION_SPECS") == "1";
		PrivateDefinitions.Add("FRAMEDASH_WITH_AUTOMATION_SPECS=" + (bBuildAutomationSpecs ? "1" : "0"));
	}
}
