// Copyright Epic Games, Inc. All Rights Reserved.

#include "WakatimeIntegration.h"
#include "Modules/ModuleManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Ticker.h"
#include <chrono>
#include "WakatimeSettings.h"
#include "ISettingsModule.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "UObject/ObjectSaveContext.h"

#define LOCTEXT_NAMESPACE "FWakatimeIntegrationModule"


IMPLEMENT_MODULE(FWakatimeIntegrationModule, WakatimeIntegration)

void FWakatimeIntegrationModule::StartupModule()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings")) {
		SettingsModule->RegisterSettings("Editor", "Plugins", "Wakatime_Settings",
			NSLOCTEXT("WakatimeIntegration", "WakatimeSettingsDisplayName", "Wakatime Integration"),
			NSLOCTEXT("WakatimeIntegration", "WakatimeSettingsDescription", "Settings for Wakatime Integration plugin"),
			GetMutableDefault<UWakatimeSettings>());
	}

	const UWakatimeSettings* Settings = GetDefault<UWakatimeSettings>();
	const float TimerDuration = Settings->WakatimeInterval;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnAssetAdded().AddRaw(this, &FWakatimeIntegrationModule::OnAssetAdded);
	AssetRegistry.OnAssetRemoved().AddRaw(this, &FWakatimeIntegrationModule::OnAssetRemoved);
	AssetRegistry.OnAssetRenamed().AddRaw(this, &FWakatimeIntegrationModule::OnAssetRenamed);

	FCoreUObjectDelegates::OnObjectSaved_EditorOnly.AddRaw(this, &FWakatimeIntegrationModule::OnObjectSaved);

	TimerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWakatimeIntegrationModule::OnTimerTick),
		TimerDuration
	);
}

void FWakatimeIntegrationModule::ShutdownModule()
{
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnAssetAdded().RemoveAll(this);
		AssetRegistry.OnAssetRemoved().RemoveAll(this);
		AssetRegistry.OnAssetRenamed().RemoveAll(this);

		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->UnregisterSettings("Editor", "Plugins", "WakatimeIntegration");
		}
	}

	FCoreUObjectDelegates::OnObjectSaved_EditorOnly.RemoveAll(this);

	FTSTicker::GetCoreTicker().RemoveTicker(TimerHandle);
}

void FWakatimeIntegrationModule::OnAssetAdded(const FAssetData& AssetData)
{
	int64 now = GetCurrentTime();
	if ((now - LastAssetPushTime) < SaveDebounce) {
		return;
	}
	LastAssetPushTime = now;

	Dirty = true;
	AddOperations++;
}

void FWakatimeIntegrationModule::OnAssetRemoved(const FAssetData& AssetData)
{
	int64 now = GetCurrentTime();
	if ((now - LastAssetPushTime) < SaveDebounce) {
		return;
	}
	LastAssetPushTime = now;

	Dirty = true;
	DeleteOperations++;
}

void FWakatimeIntegrationModule::OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath)
{
	int64 now = GetCurrentTime();
	if ((now - LastAssetPushTime) < SaveDebounce) {
		return;
	}
	LastAssetPushTime = now;

	Dirty = true;
	RenameOperations++;
}

void FWakatimeIntegrationModule::OnObjectSaved(UObject* SavedObject, FObjectPreSaveContext SaveContext)
{
	int64 now = GetCurrentTime();
	if ((now - LastAssetPushTime) < SaveDebounce) {
		return;
	}
	LastAssetPushTime = now;

	SaveOperations++;
	Dirty = true;
	LastSavedName = SavedObject->GetFName();
}

FString GetCurrentOSName()
{
#if PLATFORM_WINDOWS
	return TEXT("windows");
#elif PLATFORM_MAC
	return TEXT("darwin");
#elif PLATFORM_IOS
	return TEXT("ios");
#elif PLATFORM_LINUX
	return TEXT("linux");
#elif PLATFORM_ANDROID
	return TEXT("android");
#else
	return TEXT("unknown");
#endif
}

bool FWakatimeIntegrationModule::OnTimerTick(float DeltaTime)
{
	SendHeartbeat();
	return true;
}

void FWakatimeIntegrationModule::SendHeartbeat()
{
	FName localLastSavedName = TEXT("None");
	int32 localDeleteOperations = 0;
	int32 localSaveOperations = 0;
	int32 localRenameOperations = 0;
	int32 localAddOperations = 0;
	bool localDirty = false;
	{
		FScopeLock Lock(&DataLock);
		localDirty = Dirty;
		if (!localDirty) {
			return;
		}
		localDeleteOperations = DeleteOperations;
		localSaveOperations = SaveOperations;
		localRenameOperations = RenameOperations;
		localAddOperations = AddOperations;
		localLastSavedName = LastSavedName;

		DeleteOperations = 0;
		SaveOperations = 0;
		RenameOperations = 0;
		AddOperations = 0;
		Dirty = false;
	}
	if (!localDirty) {
		return;
	}
	const UWakatimeSettings* Settings = GetDefault<UWakatimeSettings>();
	if (!Settings) {
		return;
	}

	FString Endpoint = Settings->WakatimeEndpoint;
	if (Endpoint.EndsWith(TEXT("/")))
	{
		Endpoint.RemoveAt(Endpoint.Len() - 1);
	}

	FString EntityName = TEXT("None");
	if (localLastSavedName.IsValid())
	{
		EntityName = localLastSavedName.ToString();
	}
	int64 localTime = GetCurrentTime();
	FString EngineVersionString = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	FString ProjectName = FApp::GetProjectName();
	FString ComputerName = FPlatformProcess::ComputerName();
	FString OSName = GetCurrentOSName();

	FString TargetURL = Endpoint + TEXT("/users/current/heartbeats");
	FString Body = FString::Printf(
		TEXT(
			"{\"type\": \"file\", \"time\" : %lld, \"project\": \"%s\", \"entity\": \"%s\", "
			"\"language\": \"UnrealEngine\", \"plugin\": \"UnrealEngine\", \"is_write\": false, "
			"\"user_agent\": \"unreal_engine/%s\", \"machine_name_id\": \"%s\", "
			"\"line_additions\": %d, \"line_deletions\": %d, \"operating_system\": \"%s\"}"
		),
		localTime, *ProjectName, *EntityName, *EngineVersionString,
		*ComputerName, localAddOperations, localDeleteOperations, *OSName);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TargetURL);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("UnrealEngine5"));

	FString RawBearerToken = Settings->WakatimeBearerToken.TrimStartAndEnd();
	FString AuthToken = FString::Printf(TEXT("Bearer %s"), *RawBearerToken);
	Request->SetHeader(TEXT("Authorization"), AuthToken);

	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindRaw(this, &FWakatimeIntegrationModule::OnHttpResponse);

	Request->ProcessRequest();
}

int64 FWakatimeIntegrationModule::GetCurrentTime()
{
	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	std::chrono::system_clock::duration duration = now.time_since_epoch();
	std::chrono::seconds seconds_duration = std::chrono::duration_cast<std::chrono::seconds>(duration);
	int64_t seconds_since_epoch = seconds_duration.count();
	return seconds_since_epoch;
}

void FWakatimeIntegrationModule::OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Wakatime Integration: Failed to establish connection to Wakatime endpoint."));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseString = Response->GetContentAsString();

	if (ResponseCode >= 200 && ResponseCode < 300) {
		UE_LOG(LogTemp, Log, TEXT("Wakatime Integration: Heartbeat accepted with code %d"), ResponseCode);
	}
	else if (ResponseCode == 401)
	{
		UE_LOG(LogTemp, Error, TEXT("Wakatime Integration: Heartbeat failed due to invalid API token (401). response: %s"), *ResponseString);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Wakatime Integration: Heartbeat failed. Code: %d. Response: %s"), ResponseCode, *ResponseString);
	}
}

#undef LOCTEXT_NAMESPACE