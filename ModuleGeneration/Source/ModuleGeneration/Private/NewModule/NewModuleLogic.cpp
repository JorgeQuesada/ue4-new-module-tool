
#include "NewModule/NewModuleLogic.h"
#include "NewModule/SNewModuleDialog.h"
#include "Logging.h"

#include "ModuleDescriptor.h"

#include "Dom/JsonValue.h"
#include "GeneralProjectSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWindow.h"
#include "Dialogs/SOutputLogDialog.h"
#include "GameProjectGenerationModule.h"

#define LOCTEXT_NAMESPACE "FModuleGenerationModule"

void NewModuleController::CreateAndShowNewModuleWindow()
{
	UE_LOG(ModuleGeneration, Log, TEXT("NewModuleController::CreateAndShowNewModuleWindow"));
	
	const FVector2D WindowSize(940, 380); // 480
	const FText WindowTitle = LOCTEXT("NewModule_Title", "New C++ Module");

	TSharedRef<SWindow> AddCodeWindow =
		SNew(SWindow)
		.Title(WindowTitle)
		.ClientSize(WindowSize)
		.SizingRule(ESizingRule::FixedSize)
		.SupportsMinimize(false).SupportsMaximize(false);

	auto NewModuleDialog =
		SNew(SNewModuleDialog)
		.ParentWindow(AddCodeWindow)
		.OnClickFinished(FOnRequestNewModule::CreateLambda([](auto Directory, auto ModuleDescriptor, auto ErrorCallback) { NewModuleModel::CreateNewModule(Directory, ModuleDescriptor, ErrorCallback); }));
	AddCodeWindow->SetContent(NewModuleDialog);
	FSlateApplication::Get().AddWindow(AddCodeWindow);
}

// Declare helper functions for NewModuleModel::CreateNewModule 
namespace
{
	class FErrorHandler
	{
	public:

		FErrorHandler(const FOnCreateNewModuleError& Callback)
			:
			Callback_(Callback)
		{}
		
		void HandleError(const FString& ErrorMessage) const
		{
			// TODO: Revert any changes
			Callback_.ExecuteIfBound(ErrorMessage);
		}

	private:

		FOnCreateNewModuleError Callback_;
	};
	
	bool CopyFiles(const FString& OutputDirectory, const FModuleDescriptor& NewModule, const FErrorHandler& ErrorHandler);
	void EnqueueSubdirectories(
		IFileManager& FileManager, 
		const FString& ModuleTemplateDirectory,
		const FString& NextRelativeDirectory, 
		TQueue<FString>& RelativeDirectoryQueue);
	TArray<FString> FindFilesInDirectory(
		IFileManager& FileManager,
		const FString& ModuleTemplateDirectory,
		const FString& NextRelativeDirectory);

	bool AddNewModuleToUProjectJsonFile(const FModuleDescriptor& NewModule, const FErrorHandler& ErrorHandler);

	bool GenerateVisualStudioSolution(const FErrorHandler& ErrorHandler);
}

void NewModuleModel::CreateNewModule(const FString& OutputDirectory, const FModuleDescriptor& NewModule, const FOnCreateNewModuleError& ErrorCallback)
{
	FErrorHandler ErrorHandler(ErrorCallback);
	FScopedSlowTask ProgressBar(3, LOCTEXT("NewModule_ProgressBar_DefaultTitle", "Creating new module files..."));
	ProgressBar.MakeDialog();
	UE_LOG(ModuleGeneration, Log, TEXT("Creating new module '%s'..."), *NewModule.Name.ToString());

	ProgressBar.EnterProgressFrame(1, LOCTEXT("NewModule_ProgressBar_CopyingFiles", "Copying files..."));
	if(!CopyFiles(OutputDirectory, NewModule, ErrorHandler))
	{
		return;
	}

	ProgressBar.EnterProgressFrame(1, LOCTEXT("NewModule_ProgressBar_WrittingProjectFiles", "Writting project files..."));
	if(!AddNewModuleToUProjectJsonFile(NewModule, ErrorHandler))
	{
		return;
	}

	ProgressBar.EnterProgressFrame(1, LOCTEXT("NewModule_ProgressBar_GeneratingVisualStudioSolutionFiles", "Generating visual studio solution..."));
	GenerateVisualStudioSolution(ErrorHandler);
}

// Define helper functions for NewModuleModel::CreateNewModule 
namespace
{
	bool CopyFiles(const FString& OutputDirectory, const FModuleDescriptor& NewModule, const FErrorHandler& ErrorHandler)
	{
		// Setup string replacements for files and folders
		FStringFormatNamedArguments WildcardsToReplace;
		WildcardsToReplace.Add("ModuleName", FStringFormatArg(NewModule.Name.ToString()));
		WildcardsToReplace.Add("Copyright", FStringFormatArg(GetDefault<UGeneralProjectSettings>()->CopyrightNotice));

		// Get path to Resources/ModuleTemplate folder
		const FString& BasePluginDirectory = IPluginManager::Get().FindPlugin("ModuleGeneration")->GetBaseDir();
		const FString ModuleTemplateDirectory =
			FPaths::Combine(BasePluginDirectory, FString("Resources"));


		// Recursively search go through each sub-directory and copy over files...
		// ... replacing {ModuleName} and {Copyright} in the files
		TQueue<FString> RelativeDirectoryQueue;
		RelativeDirectoryQueue.Enqueue(FString("{ModuleName}"));
		IFileManager& FileManager = IFileManager::Get();

		while (!RelativeDirectoryQueue.IsEmpty())
		{
			const FString NextRelativeDirectory = [&RelativeDirectoryQueue]()
			{
				FString result;
				RelativeDirectoryQueue.Dequeue(result);
				return result;
			}();
			EnqueueSubdirectories(FileManager, ModuleTemplateDirectory, NextRelativeDirectory, RelativeDirectoryQueue);

			const TArray<FString> FilesInDirectory =
				FindFilesInDirectory(FileManager, ModuleTemplateDirectory, NextRelativeDirectory);

			const FString NewDirectoryName = FPaths::Combine(OutputDirectory, NextRelativeDirectory);
			const FString FormattedNewDirectoryName = FString::Format(*NewDirectoryName, WildcardsToReplace);
			if (!FileManager.DirectoryExists(*FormattedNewDirectoryName))
			{
				FileManager.MakeDirectory(*FormattedNewDirectoryName);
			}

			for (const auto FileToCopy : FilesInDirectory)
			{
				const FString FullFilePath = FPaths::Combine(ModuleTemplateDirectory, NextRelativeDirectory, FileToCopy);

				FString FileContents;
				TSharedPtr<FArchive> FileReadStream(FileManager.CreateFileReader(*FullFilePath));
				if (FileReadStream == nullptr)
				{
					ErrorHandler.HandleError(FString::Printf(TEXT("Failed to create file stream to template file '%s'"), *FullFilePath));
					return false;
				}

				const bool bCouldReadFile = FFileHelper::LoadFileToString(FileContents, *FileReadStream.Get());
				if (!bCouldReadFile)
				{
					ErrorHandler.HandleError(FString::Printf(TEXT("Failed to read template file '%s'"), *FullFilePath));
					return false;
				}

				const FString NewFileName = FString::Format(*FileToCopy, WildcardsToReplace);
				const FString NewFileFullPath = FPaths::Combine(FormattedNewDirectoryName, NewFileName);
				const FString NewFileContents = FString::Format(*FileContents, WildcardsToReplace);
				const bool bCouldWriteFile = FFileHelper::SaveStringToFile(NewFileContents, *NewFileFullPath);
				if (!bCouldWriteFile)
				{
					ErrorHandler.HandleError(FString::Printf(TEXT("Failed to write new to file '%s'"), *NewFileContents));
					return false;
				}
			}
		}

		return true;
	}
	void EnqueueSubdirectories(IFileManager& FileManager, const FString& ModuleTemplateDirectory, const FString& NextRelativeDirectory, TQueue<FString>& RelativeDirectoryQueue)
	{
		const TArray<FString> Subdirectories = [&ModuleTemplateDirectory, &NextRelativeDirectory, &FileManager]()
		{
			TArray<FString> result;
			const FString FolderWildcard("*");
			const FString DirectoryWithWildcard = FPaths::Combine(ModuleTemplateDirectory, NextRelativeDirectory, FolderWildcard);
			FileManager.FindFiles(result, *DirectoryWithWildcard, false, true);
			return result;
		}();
		for (const auto Subdirectory : Subdirectories)
		{
			const FString RelativeDirectory = FPaths::Combine(NextRelativeDirectory, Subdirectory);
			RelativeDirectoryQueue.Enqueue(RelativeDirectory);
		}
	}
	TArray<FString> FindFilesInDirectory(IFileManager& FileManager, const FString& ModuleTemplateDirectory, const FString& NextRelativeDirectory)
	{
		TArray<FString> result;
		const FString FileWildcard("*.*");
		const FString DirectoryWithWildcard = FPaths::Combine(ModuleTemplateDirectory, NextRelativeDirectory, FileWildcard);
		FileManager.FindFiles(result, *DirectoryWithWildcard, true, false);
		return result;
	}

	bool AddNewModuleToUProjectJsonFile(const FModuleDescriptor& NewModule, const FErrorHandler& ErrorHandler)
	{
		IFileManager& FileManager = IFileManager::Get();
		
		const FString ProjectDirectory = UKismetSystemLibrary::GetProjectDirectory();
		const FString ProjectSearchRegex = FPaths::Combine(ProjectDirectory, FString("*.uproject"));
		TArray<FString> UProjectFileNames;
		FileManager.FindFiles(UProjectFileNames, *ProjectSearchRegex, true, false);

		if(UProjectFileNames.Num() == 0)
		{
			ErrorHandler.HandleError(FString::Printf(TEXT("Failed to locate .uproject file for current project. Searched path: '%s'"), *ProjectSearchRegex));
			return false;
		}
		if(UProjectFileNames.Num() > 1)
		{
			UE_LOG(ModuleGeneration, Warning, TEXT("Found multiple .uproject files. Picking '%s'..."), *UProjectFileNames[0]);
		}

		const FString PathToProjectFile = FPaths::Combine(ProjectDirectory, UProjectFileNames[0]);
		FString FileContents;
		TSharedPtr<FArchive> FileReadStream(FileManager.CreateFileReader(*PathToProjectFile));
		if (FileReadStream == nullptr)
		{
			ErrorHandler.HandleError(FString::Printf(TEXT("Failed to create file stream to .uproject file '%s'"), *PathToProjectFile));
			return false;
		}
		const bool bCouldReadFile = FFileHelper::LoadFileToString(FileContents, *FileReadStream.Get());
		if (!bCouldReadFile)
		{
			ErrorHandler.HandleError(FString::Printf(TEXT("Failed to read .uproject file '%s'"), *PathToProjectFile));
			return false;
		}

		TSharedPtr<FJsonObject> ProjectFileAsJson;
		TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(*FileContents);
		if (!FJsonSerializer::Deserialize(JsonReader, ProjectFileAsJson))
		{
			ErrorHandler.HandleError(FString::Printf(TEXT("Failed to parse JSON from .uproject file '%s'"), *PathToProjectFile));
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Modules = ProjectFileAsJson->GetArrayField("Modules");
		const bool bModuleAlreadyInList = [&Modules, &NewModule]()
		{
			for(auto e : Modules)
			{
				const TSharedPtr<FJsonObject>* PtrToJsonObject;
				e->TryGetObject(PtrToJsonObject);
				if(PtrToJsonObject == nullptr)
				{
					continue;
				}

				TSharedPtr<FJsonObject> AsJsonObject = *PtrToJsonObject;
				FString ModuleName;
				if(AsJsonObject->TryGetStringField("Name", ModuleName)
					&& ModuleName.Equals(NewModule.Name.ToString()))
				{
					return true;
				}
			}
			return false;
		}();
		if(bModuleAlreadyInList)
		{
			ErrorHandler.HandleError(FString::Printf(TEXT("The .uproject file already contained an entry '%s'"), *NewModule.Name.ToString()));
			return false;
		}

		TSharedPtr<FJsonObject> ModuleAsJson(new FJsonObject);
		ModuleAsJson->SetStringField("Name", NewModule.Name.ToString());
		ModuleAsJson->SetStringField("Type", EHostType::ToString(NewModule.Type));
		ModuleAsJson->SetStringField("LoadingPhase", ELoadingPhase::ToString(NewModule.LoadingPhase));
		Modules.Add(TSharedPtr<FJsonValueObject>(new FJsonValueObject(ModuleAsJson)));
		ProjectFileAsJson->SetArrayField("Modules", Modules);

		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(ProjectFileAsJson.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(OutputString, *PathToProjectFile);
		
		return true;
	}

	bool GenerateVisualStudioSolution(const FErrorHandler& ErrorHandler)
	{
		FText FailReason, FailLog;
		if (!FGameProjectGenerationModule::Get().UpdateCodeProject(FailReason, FailLog))
		{
			ErrorHandler.HandleError(FailReason.ToString());
			return false;
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE