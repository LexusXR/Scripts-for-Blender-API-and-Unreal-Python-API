#include "BlenderToUEImporterModule.h"
#include "BlenderToUEImporterSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FBlenderToUEImporterModule"

namespace BlenderToUEImporter
{
	static const TCHAR* ImportRunningBlendEntryName = TEXT("BlenderToUEImporter.ImportFromRunningBlender");
	static const TCHAR* LiveBridgeStatusDynamicEntryName = TEXT("BlenderToUEImporter.LiveBridgeStatus");
	static const TCHAR* MenuSectionName = TEXT("BlenderToUEImporterSection");

	static FString GetDestinationRoot()
	{
		const UBlenderToUEImporterSettings* Settings = GetDefault<UBlenderToUEImporterSettings>();
		FString DestinationRoot = Settings != nullptr ? Settings->DestinationRoot : TEXT("/Game/ImportedModels");
		if (!DestinationRoot.StartsWith(TEXT("/Game")))
		{
			DestinationRoot = TEXT("/Game/ImportedModels");
		}
		DestinationRoot.RemoveFromEnd(TEXT("/"));
		return DestinationRoot;
	}

	struct FTextureSet
	{
		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Roughness = nullptr;
		UTexture* Metallic = nullptr;
		UTexture* AmbientOcclusion = nullptr;
		UTexture* ORM = nullptr;
	};

	enum class ETextureSemantic : uint8
	{
		Unknown,
		BaseColor,
		Normal,
		Roughness,
		Metallic,
		AmbientOcclusion,
		ORM
	};

	static FString NormalizeKeyForMatch(FString Value)
	{
		Value = Value.ToLower();
		Value.ReplaceInline(TEXT(" "), TEXT(""));
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		Value.ReplaceInline(TEXT("."), TEXT("_"));
		if (Value.StartsWith(TEXT("mi_")))
		{
			Value.RightChopInline(3);
		}
		else if (Value.StartsWith(TEXT("m_")))
		{
			Value.RightChopInline(2);
		}
		Value.ReplaceInline(TEXT("_"), TEXT(""));
		return Value;
	}

	static ETextureSemantic GetTextureSemantic(const FString& TextureName)
	{
		const FString Name = TextureName.ToLower();
		if (Name.Contains(TEXT("occlusionroughnessmetallic")) || Name.Contains(TEXT("_orm")) || Name.EndsWith(TEXT("orm")))
		{
			return ETextureSemantic::ORM;
		}
		if (Name.Contains(TEXT("basecolor")) || Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("diffuse")))
		{
			return ETextureSemantic::BaseColor;
		}
		if (Name.Contains(TEXT("normal")) || Name.EndsWith(TEXT("_n")) || Name.Contains(TEXT("_nrm")))
		{
			return ETextureSemantic::Normal;
		}
		if (Name.Contains(TEXT("rough")))
		{
			return ETextureSemantic::Roughness;
		}
		if (Name.Contains(TEXT("metal")))
		{
			return ETextureSemantic::Metallic;
		}
		if (Name.Contains(TEXT("ambientocclusion")) || Name.Contains(TEXT("_ao")) || Name.EndsWith(TEXT("ao")) || Name.Contains(TEXT("occlusion")))
		{
			return ETextureSemantic::AmbientOcclusion;
		}
		return ETextureSemantic::Unknown;
	}

	static bool MatchesAnyPattern(const FString& Name, const TArray<FString>& Patterns)
	{
		for (const FString& Pattern : Patterns)
		{
			if (!Pattern.IsEmpty() && Name.MatchesWildcard(Pattern, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static ETextureSemantic GetTextureSemanticWithSettings(const FString& TextureName, const UBlenderToUEImporterSettings* Settings)
	{
		if (Settings == nullptr)
		{
			return GetTextureSemantic(TextureName);
		}

		const FString Name = TextureName.ToLower();
		if (MatchesAnyPattern(Name, Settings->ORMPatterns))
		{
			return ETextureSemantic::ORM;
		}
		if (MatchesAnyPattern(Name, Settings->BaseColorPatterns))
		{
			return ETextureSemantic::BaseColor;
		}
		if (MatchesAnyPattern(Name, Settings->NormalPatterns))
		{
			return ETextureSemantic::Normal;
		}
		if (MatchesAnyPattern(Name, Settings->RoughnessPatterns))
		{
			return ETextureSemantic::Roughness;
		}
		if (MatchesAnyPattern(Name, Settings->MetallicPatterns))
		{
			return ETextureSemantic::Metallic;
		}
		if (MatchesAnyPattern(Name, Settings->AmbientOcclusionPatterns))
		{
			return ETextureSemantic::AmbientOcclusion;
		}

		return GetTextureSemantic(TextureName);
	}

	static FString DeriveMaterialKeyFromTextureName(FString TextureName)
	{
		FString Key = TextureName.ToLower();
		TArray<FString> Suffixes = {
			TEXT("occlusionroughnessmetallic"),
			TEXT("basecolor"),
			TEXT("albedo"),
			TEXT("diffuse"),
			TEXT("normal"),
			TEXT("roughness"),
			TEXT("rough"),
			TEXT("metallic"),
			TEXT("metal"),
			TEXT("ambientocclusion"),
			TEXT("occlusion"),
			TEXT("orm"),
			TEXT("ao"),
			TEXT("nrm")
		};

		for (const FString& Suffix : Suffixes)
		{
			const FString TokenUnderscore = FString::Printf(TEXT("_%s"), *Suffix);
			const FString TokenDash = FString::Printf(TEXT("-%s"), *Suffix);
			Key.ReplaceInline(*TokenUnderscore, TEXT(""));
			Key.ReplaceInline(*TokenDash, TEXT(""));
			if (Key.EndsWith(Suffix))
			{
				Key.LeftChopInline(Suffix.Len());
			}
		}

		Key.ReplaceInline(TEXT("__"), TEXT("_"));
		Key.RemoveFromEnd(TEXT("_"));
		Key.RemoveFromStart(TEXT("_"));
		return NormalizeKeyForMatch(Key);
	}

	static void AssignTextureBySemantic(FTextureSet& TextureSet, UTexture* Texture, ETextureSemantic Semantic)
	{
		switch (Semantic)
		{
		case ETextureSemantic::BaseColor:
			if (TextureSet.BaseColor == nullptr) TextureSet.BaseColor = Texture;
			break;
		case ETextureSemantic::Normal:
			if (TextureSet.Normal == nullptr) TextureSet.Normal = Texture;
			break;
		case ETextureSemantic::Roughness:
			if (TextureSet.Roughness == nullptr) TextureSet.Roughness = Texture;
			break;
		case ETextureSemantic::Metallic:
			if (TextureSet.Metallic == nullptr) TextureSet.Metallic = Texture;
			break;
		case ETextureSemantic::AmbientOcclusion:
			if (TextureSet.AmbientOcclusion == nullptr) TextureSet.AmbientOcclusion = Texture;
			break;
		case ETextureSemantic::ORM:
			if (TextureSet.ORM == nullptr) TextureSet.ORM = Texture;
			break;
		case ETextureSemantic::Unknown:
		default:
			break;
		}
	}

	static bool HasAnyTexture(const FTextureSet& TextureSet)
	{
		return TextureSet.BaseColor != nullptr
			|| TextureSet.Normal != nullptr
			|| TextureSet.Roughness != nullptr
			|| TextureSet.Metallic != nullptr
			|| TextureSet.AmbientOcclusion != nullptr
			|| TextureSet.ORM != nullptr;
	}

	struct FTextureBuckets
	{
		FTextureSet Global;
		TMap<FString, FTextureSet> ByMaterialKey;
	};

	static FTextureBuckets BuildTextureBuckets(const TArray<UTexture*>& Textures, const UBlenderToUEImporterSettings* Settings)
	{
		FTextureBuckets Buckets;
		for (UTexture* Texture : Textures)
		{
			if (Texture == nullptr)
			{
				continue;
			}
			const FString Name = Texture->GetName();
			const ETextureSemantic Semantic = GetTextureSemanticWithSettings(Name, Settings);
			AssignTextureBySemantic(Buckets.Global, Texture, Semantic);

			const FString MaterialKey = DeriveMaterialKeyFromTextureName(Name);
			if (!MaterialKey.IsEmpty())
			{
				FTextureSet& MaterialSet = Buckets.ByMaterialKey.FindOrAdd(MaterialKey);
				AssignTextureBySemantic(MaterialSet, Texture, Semantic);
			}
		}
		return Buckets;
	}

	static const FTextureSet* FindTextureSetForSlot(const FTextureBuckets& Buckets, const FString& SlotName, const FString& ImportedSlotName, bool bAllowGlobalFallback)
	{
		auto ScoreKey = [&](const FString& CandidateRaw) -> const FTextureSet*
		{
			const FString Candidate = NormalizeKeyForMatch(CandidateRaw);
			if (Candidate.IsEmpty())
			{
				return nullptr;
			}

			const FTextureSet* Best = nullptr;
			int32 BestScore = 0;
			for (const TPair<FString, FTextureSet>& Pair : Buckets.ByMaterialKey)
			{
				if (!HasAnyTexture(Pair.Value))
				{
					continue;
				}
				int32 Score = 0;
				if (Pair.Key == Candidate)
				{
					Score = 100;
				}
				else if (Pair.Key.Contains(Candidate) || Candidate.Contains(Pair.Key))
				{
					Score = 50;
				}
				if (Score > BestScore)
				{
					BestScore = Score;
					Best = &Pair.Value;
				}
			}
			return Best;
		};

		if (const FTextureSet* ExactImported = ScoreKey(ImportedSlotName))
		{
			return ExactImported;
		}
		if (const FTextureSet* ExactSlot = ScoreKey(SlotName))
		{
			return ExactSlot;
		}
		if (bAllowGlobalFallback && HasAnyTexture(Buckets.Global))
		{
			return &Buckets.Global;
		}
		return nullptr;
	}

	static void SetTextureParameterIfValid(UMaterialInstanceConstant* MaterialInstance, FName ParameterName, UTexture* Texture)
	{
		if (MaterialInstance != nullptr && Texture != nullptr && !ParameterName.IsNone())
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ParameterName), Texture);
		}
	}

	static FString SanitizeAssetSuffix(const FString& InSuffix)
	{
		FString Result = InSuffix;
		Result = ObjectTools::SanitizeObjectName(Result);
		return Result.IsEmpty() ? TEXT("Slot") : Result;
	}

	static void ApplyTextureSetToMaterialInstance(UMaterialInstanceConstant* MaterialInstance, UMaterialInterface* ParentMaterial, const UBlenderToUEImporterSettings* Settings, const FTextureSet& TextureSet)
	{
		if (MaterialInstance == nullptr || ParentMaterial == nullptr || Settings == nullptr)
		{
			return;
		}

		MaterialInstance->SetParentEditorOnly(ParentMaterial, true);
		SetTextureParameterIfValid(MaterialInstance, Settings->BaseColorParameterName, TextureSet.BaseColor);
		SetTextureParameterIfValid(MaterialInstance, Settings->NormalParameterName, TextureSet.Normal);

		if (Settings->bUsePackedORMTexture && TextureSet.ORM != nullptr)
		{
			SetTextureParameterIfValid(MaterialInstance, Settings->ORMParameterName, TextureSet.ORM);
		}
		else
		{
			SetTextureParameterIfValid(MaterialInstance, Settings->RoughnessParameterName, TextureSet.Roughness);
			SetTextureParameterIfValid(MaterialInstance, Settings->MetallicParameterName, TextureSet.Metallic);
			SetTextureParameterIfValid(MaterialInstance, Settings->AmbientOcclusionParameterName, TextureSet.AmbientOcclusion);
		}

		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();
	}

	static FString ToObjectPath(const FString& AssetPath)
	{
		if (AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static FString ToScriptExportMode(EBlenderToUEExportMode ExportMode)
	{
		switch (ExportMode)
		{
		case EBlenderToUEExportMode::MergeSelected:
			return TEXT("merge_selected");
		case EBlenderToUEExportMode::BatchSelected:
			return TEXT("batch_selected");
		case EBlenderToUEExportMode::ActiveOnly:
		default:
			return TEXT("active_only");
		}
	}
}

void FBlenderToUEImporterModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlenderToUEImporterModule::RegisterMenus));
}

void FBlenderToUEImporterModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FBlenderToUEImporterModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	FToolMenuSection& Section = Menu->FindOrAddSection(BlenderToUEImporter::MenuSectionName);

	Section.AddMenuEntry(
		BlenderToUEImporter::ImportRunningBlendEntryName,
		LOCTEXT("ImportFromRunningBlenderLabel", "Import From Running Blender"),
		LOCTEXT("ImportFromRunningBlenderTooltip", "Export active mesh from live Blender session via local bridge addon."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FBlenderToUEImporterModule::ImportFromRunningBlender)));

	Section.AddDynamicEntry(
		BlenderToUEImporter::LiveBridgeStatusDynamicEntryName,
		FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
		{
			FString Details;
			const bool bConnected = IsRunningBlenderBridgeAvailable(&Details);
			const FText Label = bConnected
				? LOCTEXT("LiveBridgeConnected", "Blender Live Bridge: Connected")
				: LOCTEXT("LiveBridgeDisconnected", "Blender Live Bridge: Disconnected");
			const FText Tooltip = bConnected
				? LOCTEXT("LiveBridgeConnectedTip", "Blender bridge is reachable at 127.0.0.1:8765.")
				: FText::Format(
					LOCTEXT("LiveBridgeDisconnectedTip", "Blender bridge is not reachable. {0}"),
					FText::FromString(Details));

			InSection.AddMenuEntry(
				TEXT("BlenderToUEImporter.LiveBridgeStatusDisplay"),
				Label,
				Tooltip,
				FSlateIcon(),
				FUIAction());
		}));
}

void FBlenderToUEImporterModule::ImportFromRunningBlender()
{
	TArray<FString> ExportedFilePaths;
	if (!RequestRunningBlenderExport(ExportedFilePaths))
	{
		return;
	}

	for (const FString& ExportedFilePath : ExportedFilePaths)
	{
		ImportFileToUnreal(ExportedFilePath);
	}
}

bool FBlenderToUEImporterModule::ImportFileToUnreal(const FString& SourceFilePath)
{
	const FString RawModelName = FPaths::GetBaseFilename(SourceFilePath);
	const FString AssetName = ObjectTools::SanitizeObjectName(RawModelName);

	if (AssetName.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("InvalidAssetName", "Could not derive a valid asset name from file name."));
		return false;
	}

	const FString ModelFolder = FString::Printf(TEXT("%s/%s"), *BlenderToUEImporter::GetDestinationRoot(), *AssetName);

	if (UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
	{
		AssetSubsystem->MakeDirectory(ModelFolder);
	}

	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->Filename = SourceFilePath;
	ImportTask->DestinationPath = ModelFolder;
	ImportTask->DestinationName = AssetName;
	ImportTask->bReplaceExisting = true;
	ImportTask->bAutomated = true;
	ImportTask->bSave = true;

	if (FPaths::GetExtension(SourceFilePath).Equals(TEXT("fbx"), ESearchCase::IgnoreCase))
	{
		const UBlenderToUEImporterSettings* Settings = GetDefault<UBlenderToUEImporterSettings>();
		UFbxImportUI* ImportOptions = NewObject<UFbxImportUI>();
		ImportOptions->bAutomatedImportShouldDetectType = false;
		ImportOptions->MeshTypeToImport = EFBXImportType::FBXIT_StaticMesh;
		ImportOptions->bImportAsSkeletal = false;
		ImportOptions->bImportMesh = true;
		ImportOptions->bImportMaterials = Settings == nullptr ? true : Settings->bImportMaterials;
		ImportOptions->bImportTextures = Settings == nullptr ? true : Settings->bImportTextures;
		if (ImportOptions->StaticMeshImportData != nullptr)
		{
			ImportOptions->StaticMeshImportData->bGenerateLightmapUVs = false;
		}

		ImportTask->Options = ImportOptions;
	}

	TArray<UAssetImportTask*> ImportTasks;
	ImportTasks.Add(ImportTask);

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetToolsModule.Get().ImportAssetTasks(ImportTasks);
	ApplyMasterMaterialPipeline(ImportTask, ModelFolder, AssetName);
	return true;
}

bool FBlenderToUEImporterModule::RequestRunningBlenderExport(TArray<FString>& OutExportedFilePaths)
{
	FString BridgeDetails;
	if (!IsRunningBlenderBridgeAvailable(&BridgeDetails))
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::Format(
				LOCTEXT("RunningBridgeUnavailable", "Could not connect to running Blender bridge.\n\n{0}"),
				FText::FromString(BridgeDetails)));
		return false;
	}

	const UBlenderToUEImporterSettings* Settings = GetDefault<UBlenderToUEImporterSettings>();
	const bool bApplyModifiers = Settings == nullptr ? true : Settings->bApplyModifiers;
	const bool bAutoGenerateUvIfMissing = Settings == nullptr ? true : Settings->bAutoGenerateUvIfMissing;
	const EBlenderToUEExportMode ExportMode = Settings == nullptr ? EBlenderToUEExportMode::ActiveOnly : Settings->ExportMode;
	const FString ScriptExportMode = BlenderToUEImporter::ToScriptExportMode(ExportMode);

	const FString ExportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlenderToUEPipeline"));
	IFileManager::Get().MakeDirectory(*ExportDirectory, true);

	const FString FileName = FString::Printf(TEXT("LiveBlend_%s.fbx"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString BaseOutputPath = FPaths::Combine(ExportDirectory, FileName);

	FString JsonOutputPath = BaseOutputPath;
	JsonOutputPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	JsonOutputPath.ReplaceInline(TEXT("\""), TEXT("\\\""));

	FString JsonMode = ScriptExportMode;
	JsonMode.ReplaceInline(TEXT("\""), TEXT("\\\""));

	const FString JsonBody = FString::Printf(
		TEXT("{\\\"output_path\\\":\\\"%s\\\",\\\"apply_modifiers\\\":%s,\\\"auto_uv\\\":%s,\\\"export_mode\\\":\\\"%s\\\"}"),
		*JsonOutputPath,
		bApplyModifiers ? TEXT("true") : TEXT("false"),
		bAutoGenerateUvIfMissing ? TEXT("true") : TEXT("false"),
		*JsonMode);

	const FString CurlParams = FString::Printf(
		TEXT("-s -X POST http://127.0.0.1:8765/export_active -H \"Content-Type: application/json\" -d \"%s\""),
		*JsonBody);

	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;
	const bool bExecOk = FPlatformProcess::ExecProcess(TEXT("curl.exe"), *CurlParams, &ReturnCode, &StdOut, &StdErr);

	if (!bExecOk || ReturnCode != 0)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(
				TEXT("Could not connect to running Blender bridge.\n\nInstall and enable BlenderLiveBridgeAddon.py in Blender, then click Start Server.\n\n")
				TEXT("Expected endpoint: http://127.0.0.1:8765/export_active")));
		return false;
	}

	StdOut = StdOut.TrimStartAndEnd();
	if (!StdOut.StartsWith(TEXT("OK|")))
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::Format(
				LOCTEXT("RunningBlenderExportFailed", "Blender bridge export failed:\n{0}"),
				FText::FromString(StdOut.IsEmpty() ? StdErr : StdOut)));
		return false;
	}

	const FString Payload = StdOut.RightChop(3).TrimStartAndEnd();
	TArray<FString> Paths;
	Payload.ParseIntoArray(Paths, TEXT("|"), true);
	if (Paths.IsEmpty())
	{
		Paths.Add(Payload);
	}

	for (FString& Path : Paths)
	{
		Path = Path.TrimStartAndEnd();
		if (!Path.IsEmpty())
		{
			OutExportedFilePaths.AddUnique(Path);
		}
	}

	if (OutExportedFilePaths.IsEmpty())
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::Format(
				LOCTEXT("RunningBlenderNoPaths", "Bridge reported success, but returned no exported files.\n\n{0}"),
				FText::FromString(StdOut)));
		return false;
	}

	for (const FString& ExportedPath : OutExportedFilePaths)
	{
		if (!FPaths::FileExists(ExportedPath))
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::Format(
					LOCTEXT("RunningBlenderNoFile", "Bridge reported success, but FBX was not found:\n{0}"),
					FText::FromString(ExportedPath)));
			return false;
		}
	}

	return true;
}

bool FBlenderToUEImporterModule::IsRunningBlenderBridgeAvailable(FString* OutDetails) const
{
	const FString CurlParams = TEXT("-s --max-time 2 http://127.0.0.1:8765/health");
	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;
	const bool bExecOk = FPlatformProcess::ExecProcess(TEXT("curl.exe"), *CurlParams, &ReturnCode, &StdOut, &StdErr);

	if (!bExecOk)
	{
		if (OutDetails != nullptr)
		{
			*OutDetails = TEXT("Failed to execute curl.exe.");
		}
		return false;
	}

	if (ReturnCode != 0)
	{
		if (OutDetails != nullptr)
		{
			*OutDetails = FString::Printf(TEXT("Endpoint http://127.0.0.1:8765/health is unreachable (curl exit code: %d)."), ReturnCode);
		}
		return false;
	}

	StdOut = StdOut.TrimStartAndEnd();
	if (!StdOut.StartsWith(TEXT("OK|")))
	{
		if (OutDetails != nullptr)
		{
			*OutDetails = FString::Printf(TEXT("Unexpected response from bridge: %s"), *(StdOut.IsEmpty() ? StdErr : StdOut));
		}
		return false;
	}

	if (OutDetails != nullptr)
	{
		*OutDetails = StdOut;
	}
	return true;
}

void FBlenderToUEImporterModule::ApplyMasterMaterialPipeline(UAssetImportTask* ImportTask, const FString& ModelFolder, const FString& AssetName)
{
	const UBlenderToUEImporterSettings* Settings = GetDefault<UBlenderToUEImporterSettings>();
	if (Settings == nullptr)
	{
		return;
	}

	UMaterialInterface* MasterMaterial = Settings->MasterMaterial.LoadSynchronous();
	if (MasterMaterial == nullptr)
	{
		return;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistryModule.Get().SearchAllAssets(true);

	TArray<UTexture*> ImportedTextures;
	{
		FARFilter TextureFilter;
		TextureFilter.PackagePaths.Add(*ModelFolder);
		TextureFilter.bRecursivePaths = true;
		TextureFilter.ClassPaths.Add(UTexture::StaticClass()->GetClassPathName());

		TArray<FAssetData> TextureAssets;
		AssetRegistryModule.Get().GetAssets(TextureFilter, TextureAssets);
		for (const FAssetData& AssetData : TextureAssets)
		{
			if (UTexture* Texture = Cast<UTexture>(AssetData.GetAsset()))
			{
				ImportedTextures.Add(Texture);
			}
		}
	}
	const BlenderToUEImporter::FTextureBuckets TextureBuckets = BlenderToUEImporter::BuildTextureBuckets(ImportedTextures, Settings);

	UE_LOG(LogTemp, Log, TEXT("BlenderToUE: Found %d textures in %s, %d material keys"),
		ImportedTextures.Num(), *ModelFolder, TextureBuckets.ByMaterialKey.Num());
	for (const TPair<FString, BlenderToUEImporter::FTextureSet>& Pair : TextureBuckets.ByMaterialKey)
	{
		UE_LOG(LogTemp, Log, TEXT("BlenderToUE:   TextureKey='%s' BC=%d N=%d R=%d M=%d AO=%d ORM=%d"),
			*Pair.Key,
			Pair.Value.BaseColor != nullptr, Pair.Value.Normal != nullptr,
			Pair.Value.Roughness != nullptr, Pair.Value.Metallic != nullptr,
			Pair.Value.AmbientOcclusion != nullptr, Pair.Value.ORM != nullptr);
	}

	if (ImportedTextures.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("BlenderToUE: No textures found, skipping Master Material pipeline"));
		return;
	}

	TArray<UStaticMesh*> ImportedMeshes;
	if (ImportTask != nullptr)
	{
		for (const FString& ImportedPath : ImportTask->ImportedObjectPaths)
		{
			if (UObject* ImportedObject = LoadObject<UObject>(nullptr, *BlenderToUEImporter::ToObjectPath(ImportedPath)))
			{
				if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(ImportedObject))
				{
					ImportedMeshes.AddUnique(StaticMesh);
				}
			}
		}
	}

	if (ImportedMeshes.IsEmpty())
	{
		FARFilter MeshFilter;
		MeshFilter.PackagePaths.Add(*ModelFolder);
		MeshFilter.bRecursivePaths = true;
		MeshFilter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());

		TArray<FAssetData> MeshAssets;
		AssetRegistryModule.Get().GetAssets(MeshFilter, MeshAssets);
		for (const FAssetData& MeshAsset : MeshAssets)
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(MeshAsset.GetAsset()))
			{
				ImportedMeshes.AddUnique(Mesh);
			}
		}
	}

	if (ImportedMeshes.IsEmpty())
	{
		return;
	}

	auto FindOrCreateMaterialInstance = [&](const FString& InstanceName, const BlenderToUEImporter::FTextureSet& TextureSet) -> UMaterialInstanceConstant*
	{
		const FString InstancePath = FString::Printf(TEXT("%s/%s.%s"), *ModelFolder, *InstanceName, *InstanceName);
		UMaterialInstanceConstant* MaterialInstance = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
		if (MaterialInstance == nullptr)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = MasterMaterial;
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
			MaterialInstance = Cast<UMaterialInstanceConstant>(
				AssetToolsModule.Get().CreateAsset(InstanceName, ModelFolder, UMaterialInstanceConstant::StaticClass(), Factory));
		}
		if (MaterialInstance == nullptr)
		{
			return nullptr;
		}

		BlenderToUEImporter::ApplyTextureSetToMaterialInstance(MaterialInstance, MasterMaterial, Settings, TextureSet);
		return MaterialInstance;
	};

	UMaterialInstanceConstant* SharedInstance = nullptr;
	if (!Settings->bCreateMaterialInstancePerSlot)
	{
		const BlenderToUEImporter::FTextureSet* SharedTextureSet = BlenderToUEImporter::HasAnyTexture(TextureBuckets.Global) ? &TextureBuckets.Global : nullptr;
		if (SharedTextureSet == nullptr)
		{
			for (const TPair<FString, BlenderToUEImporter::FTextureSet>& Pair : TextureBuckets.ByMaterialKey)
			{
				if (BlenderToUEImporter::HasAnyTexture(Pair.Value))
				{
					SharedTextureSet = &Pair.Value;
					break;
				}
			}
		}
		if (SharedTextureSet != nullptr)
		{
			const FString SharedName = FString::Printf(TEXT("MI_%s_Auto"), *AssetName);
			SharedInstance = FindOrCreateMaterialInstance(SharedName, *SharedTextureSet);
		}
	}

	for (UStaticMesh* Mesh : ImportedMeshes)
	{
		const int32 MaterialCount = Mesh->GetStaticMaterials().Num();
		const bool bAllowGlobalFallback = (MaterialCount <= 1);
		UE_LOG(LogTemp, Log, TEXT("BlenderToUE: Mesh '%s' has %d material slot(s), globalFallback=%d"),
			*Mesh->GetName(), MaterialCount, bAllowGlobalFallback);

		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			if (Settings->bCreateMaterialInstancePerSlot)
			{
				const FStaticMaterial& StaticMaterial = Mesh->GetStaticMaterials()[MaterialIndex];
				const FString SlotName = StaticMaterial.MaterialSlotName.ToString();
				const FString ImportedSlotName = StaticMaterial.ImportedMaterialSlotName.ToString();
				const BlenderToUEImporter::FTextureSet* SlotTextureSet = BlenderToUEImporter::FindTextureSetForSlot(TextureBuckets, SlotName, ImportedSlotName, bAllowGlobalFallback);

				if (SlotTextureSet != nullptr && BlenderToUEImporter::HasAnyTexture(*SlotTextureSet))
				{
					const FString Suffix = !ImportedSlotName.IsEmpty() ? ImportedSlotName : SlotName;
					const FString InstanceName = FString::Printf(
						TEXT("MI_%s_%s_Auto"),
						*AssetName,
						*BlenderToUEImporter::SanitizeAssetSuffix(Suffix));

					if (UMaterialInstanceConstant* SlotInstance = FindOrCreateMaterialInstance(InstanceName, *SlotTextureSet))
					{
						Mesh->SetMaterial(MaterialIndex, SlotInstance);
						UE_LOG(LogTemp, Log, TEXT("BlenderToUE:   Slot[%d] '%s' (imported='%s') -> MI '%s'"),
							MaterialIndex, *SlotName, *ImportedSlotName, *InstanceName);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("BlenderToUE:   Slot[%d] '%s' (imported='%s') -> NO texture match, keeping FBX material"),
						MaterialIndex, *SlotName, *ImportedSlotName);
				}
			}
			else if (SharedInstance != nullptr)
			{
				Mesh->SetMaterial(MaterialIndex, SharedInstance);
			}
		}
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlenderToUEImporterModule, BlenderToUEImporter)
