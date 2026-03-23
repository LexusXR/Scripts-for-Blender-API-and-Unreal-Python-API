#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BlenderToUEImporterSettings.generated.h"

class UMaterialInterface;

UENUM()
enum class EBlenderToUEExportMode : uint8
{
	ActiveOnly UMETA(DisplayName="Active Mesh Only"),
	MergeSelected UMETA(DisplayName="Merge Selected Meshes"),
	BatchSelected UMETA(DisplayName="Batch Export Selected Meshes")
};

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Blender To UE Importer"))
class BLENDERTOUEIMPORTER_API UBlenderToUEImporterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override
	{
		return TEXT("Plugins");
	}

	UPROPERTY(Config, EditAnywhere, Category="Import", meta=(ToolTip="Content path where imported models are stored. Must start with /Game."))
	FString DestinationRoot = TEXT("/Game/ImportedModels");

	UPROPERTY(Config, EditAnywhere, Category="Blender Pipeline")
	bool bApplyModifiers = true;

	UPROPERTY(Config, EditAnywhere, Category="Blender Pipeline")
	bool bAutoGenerateUvIfMissing = true;

	UPROPERTY(Config, EditAnywhere, Category="Blender Pipeline")
	EBlenderToUEExportMode ExportMode = EBlenderToUEExportMode::ActiveOnly;

	UPROPERTY(Config, EditAnywhere, Category="Unreal Import")
	bool bImportMaterials = true;

	UPROPERTY(Config, EditAnywhere, Category="Unreal Import")
	bool bImportTextures = true;

	UPROPERTY(Config, EditAnywhere, Category="Master Material", meta=(ToolTip="Optional master material used to create and assign material instances after import."))
	TSoftObjectPtr<UMaterialInterface> MasterMaterial;

	UPROPERTY(Config, EditAnywhere, Category="Master Material")
	bool bCreateMaterialInstancePerSlot = true;

	UPROPERTY(Config, EditAnywhere, Category="Master Material")
	bool bUsePackedORMTexture = false;

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName BaseColorParameterName = TEXT("BaseColor");

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName NormalParameterName = TEXT("Normal");

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName RoughnessParameterName = TEXT("Roughness");

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName MetallicParameterName = TEXT("Metallic");

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName AmbientOcclusionParameterName = TEXT("AmbientOcclusion");

	UPROPERTY(Config, EditAnywhere, Category="Master Material|Parameters")
	FName ORMParameterName = TEXT("ORM");

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns", meta=(ToolTip="Wildcard patterns for BaseColor textures, e.g. *basecolor*"))
	TArray<FString> BaseColorPatterns = { TEXT("*basecolor*"), TEXT("*albedo*"), TEXT("*diffuse*") };

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns", meta=(ToolTip="Wildcard patterns for Normal textures, e.g. *normal*"))
	TArray<FString> NormalPatterns = { TEXT("*normal*"), TEXT("*_n*"), TEXT("*_nrm*") };

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns")
	TArray<FString> RoughnessPatterns = { TEXT("*roughness*"), TEXT("*rough*") };

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns")
	TArray<FString> MetallicPatterns = { TEXT("*metallic*"), TEXT("*metal*") };

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns")
	TArray<FString> AmbientOcclusionPatterns = { TEXT("*ambientocclusion*"), TEXT("*occlusion*"), TEXT("*_ao*"), TEXT("*ao") };

	UPROPERTY(Config, EditAnywhere, Category="Texture Matching|Patterns")
	TArray<FString> ORMPatterns = { TEXT("*occlusionroughnessmetallic*"), TEXT("*_orm*"), TEXT("*orm") };
};
