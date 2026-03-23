#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FBlenderToUEImporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void ImportFromRunningBlender();
	bool ImportFileToUnreal(const FString& SourceFilePath);
	bool RequestRunningBlenderExport(TArray<FString>& OutExportedFilePaths);
	bool IsRunningBlenderBridgeAvailable(FString* OutDetails = nullptr) const;
	void ApplyMasterMaterialPipeline(class UAssetImportTask* ImportTask, const FString& ModelFolder, const FString& AssetName);
};
