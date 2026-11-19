#if defined(__DAVAENGINE_BEAST__)

#include "Classes/Beast/Private/BeastRunner.h"
#include "Classes/Beast/Private/LightmapsPacker.h"

#include <REPlatform/Scene/Utils/Utils.h>

#include <Beast/SceneParser.h>

#include <Time/SystemTimer.h>
#include <Scene3D/Scene.h>
#include <FileSystem/FileSystem.h>

//Beast
BeastRunner::BeastRunner(DAVA::Scene* scene, const DAVA::FilePath& scenePath_, const DAVA::FilePath& outputPath_, Beast::eBeastMode mode, std::unique_ptr<DAVA::WaitHandle> waitHandle_)
    : beastManager(new Beast::BeastManager())
    , waitHandle(std::move(waitHandle_))
    , workingScene(scene)
    , scenePath(scenePath_)
    , outputPath(outputPath_)
    , beastMode(mode)
{
    outputPath.MakeDirectoryPathname();
    beastManager->SetMode(beastMode);
}

void BeastRunner::RunUIMode()
{
    Start();

    while (Process() == false)
    {
        if (cancelledManually)
        {
            beastManager->Cancel();
            break;
        }
        else if (waitHandle)
        {
            waitHandle->SetProgressValue(beastManager->GetCurTaskProcess());
            waitHandle->SetMessage(beastManager->GetCurTaskName().c_str());
            cancelledManually |= waitHandle->WasCanceled();
        }

        Sleep(15);
    }

    Finish(cancelledManually || beastManager->WasCancelled());

    if (waitHandle)
    {
        waitHandle.reset();
    }
}

void BeastRunner::Start()
{
    startTime = DAVA::SystemTimer::GetMs();

    DAVA::FilePath path = GetLightmapDirectoryPath();
    if (beastMode == Beast::eBeastMode::MODE_LIGHTMAPS)
    {
        DAVA::FileSystem::Instance()->CreateDirectory(path, false);
        DAVA::FileSystem::Instance()->CreateDirectory(outputPath, true);
    }

    beastManager->SetLightmapsDirectory(path.GetAbsolutePathname());
    beastManager->Run(workingScene);
}

bool BeastRunner::Process()
{
    beastManager->Update();
    return beastManager->IsJobDone();
}

void BeastRunner::Finish(bool canceled)
{
    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    const bool produced = beastManager->HasProducedOutput();

    if (!canceled && produced && beastMode == Beast::eBeastMode::MODE_LIGHTMAPS)
    {
        PackLightmaps();
    }
    else if (!produced)
    {
        // packing an empty directory only produced "rename failed ... No such file
        // or directory" noise on top of the real error, which is always logged above
        DAVA::Logger::Error("[BeastRunner] the bake produced no output, nothing was packed. "
                            "The renderer's own messages are in beast_info.log / beast_error.log next to the render output.");
    }

    // release Beast's own log handles before the directories holding them are
    // removed, otherwise the delete fails with "Permission denied"
    beastManager->DisableLogTargets();

    if (beastMode == Beast::eBeastMode::MODE_LIGHTMAPS)
    {
        // keep the renderer log with the results instead of deleting it with the scratch dir
        static const char* logNames[] = { "beast_error.log", "beast_info.log" };
        for (const char* logName : logNames)
        {
            DAVA::FilePath source = GetLightmapDirectoryPath() + logName;
            if (fs->IsFile(source))
            {
                fs->MoveFile(source, outputPath + logName, true);
            }
        }
    }

    // A finished bake used to throw away the two things that make it explainable: the job folder
    // (target<M>_pass<P>.xml, the only map from a target number to the instance it belongs to)
    // and Ernst's *.dmp. Both live under the scratch directory deleted on the next line, which is
    // why "repack without baking" and "why is this wall black" had no data to answer with.
    {
        const DAVA::FilePath jobsRoot = Beast::SceneParser::GetTemporaryFolder() + "cache/jobs/";
        const DAVA::FilePath artifactsDir = outputPath + "bake-artifacts/";
        DAVA::int32 salvaged = 0;
        if (fs->IsDirectory(jobsRoot))
        {
            fs->CreateDirectory(artifactsDir, true);

            DAVA::FileList* jobs = new DAVA::FileList(jobsRoot);
            for (DAVA::int32 j = 0; j < jobs->GetCount(); ++j)
            {
                if (!jobs->IsDirectory(j))
                {
                    continue;
                }

                const DAVA::FilePath& jobDir = jobs->GetPathname(j);
                DAVA::FileList* jobFiles = new DAVA::FileList(jobDir);
                for (DAVA::int32 k = 0; k < jobFiles->GetCount(); ++k)
                {
                    if (jobFiles->IsDirectory(k))
                    {
                        continue;
                    }

                    const DAVA::FilePath& jobFile = jobFiles->GetPathname(k);
                    if (!jobFile.IsEqualToExtension(".xml") && !jobFile.IsEqualToExtension(".dmp") && !jobFile.IsEqualToExtension(".txt"))
                    {
                        continue;
                    }

                    fs->MoveFile(jobFile, artifactsDir + jobFile.GetFilename(), true);
                    ++salvaged;
                }
                jobFiles->Release();
            }
            jobs->Release();
        }
        DAVA::Logger::Info("[Beast] kept %d job artefacts (target map, minidumps) in %s", salvaged, artifactsDir.GetAbsolutePathname().c_str());
    }

    fs->DeleteDirectory(Beast::SceneParser::GetTemporaryFolder());
    if (beastMode == Beast::eBeastMode::MODE_LIGHTMAPS)
    {
        fs->DeleteDirectory(GetLightmapDirectoryPath());
    }
}

void BeastRunner::PackLightmaps()
{
    DAVA::FilePath inputDir = GetLightmapDirectoryPath();
    DAVA::FilePath outputDir = outputPath;

    DAVA::FileSystem* fs = DAVA::FileSystem::Instance();
    const DAVA::FilePath landscapeInput = inputDir + "landscape.png";
    const bool hasLandscape = fs->IsFile(landscapeInput);

    if (hasLandscape)
    {
        fs->MoveFile(landscapeInput, scenePath.GetDirectory() + "temp_landscape_lightmap.png", true);
    }

    LightmapsPacker packer;
    packer.SetInputDir(inputDir);

    packer.SetOutputDir(outputDir);
    packer.PackLightmaps(DAVA::GPU_ORIGIN);
    packer.CreateDescriptors();
    packer.ParseSpriteDescriptors();

    beastManager->UpdateAtlas(packer.GetAtlasingData());

    if (hasLandscape)
    {
        fs->MoveFile(scenePath.GetDirectory() + "temp_landscape_lightmap.png", outputDir + "landscape.png", true);
    }
    else
    {
        DAVA::Logger::Warning("[BeastRunner] no landscape.png in the render output, the landscape was not baked in this pass");
    }
    fs->DeleteDirectory(scenePath.GetDirectory() + "$process/");
}

DAVA::FilePath BeastRunner::GetLightmapDirectoryPath() const
{
    return scenePath + "_beast/";
}


#endif //#if defined (__DAVAENGINE_BEAST__)
