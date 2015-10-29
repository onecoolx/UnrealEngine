// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "MovieSceneTracksPrivatePCH.h"
#include "MovieSceneShotTrack.h"
#include "MovieSceneShotTrackInstance.h"
#include "MovieSceneShotSection.h"
#include "IMovieScenePlayer.h"


/* FMovieSceneShotTrackInstance structors
 *****************************************************************************/

FMovieSceneShotTrackInstance::FMovieSceneShotTrackInstance(UMovieSceneShotTrack& InShotTrack)
	: ShotTrack(&InShotTrack)
{ }


/* IMovieSceneTrackInstance interface
 *****************************************************************************/

void FMovieSceneShotTrackInstance::ClearInstance(IMovieScenePlayer& Player, FMovieSceneSequenceInstance& SequenceInstance)
{
	Player.UpdateCameraCut(nullptr, false);
}


void FMovieSceneShotTrackInstance::RefreshInstance(const TArray<UObject*>& RuntimeObjects, IMovieScenePlayer& Player, FMovieSceneSequenceInstance& SequenceInstance)
{
	const TArray<UMovieSceneSection*>& ShotSections = ShotTrack->GetAllSections();

	RuntimeCameraObjects.Empty(ShotSections.Num());

	for (UMovieSceneSection* Section : ShotSections)
	{
		// @todo Sequencer - Sub-moviescenes: Get the cameras from the root movie scene instance.  We should support adding cameras for sub-moviescenes as shots
		TArray<UObject*> CameraObjects;
		Player.GetRuntimeObjects(Player.GetRootMovieSceneSequenceInstance(), CastChecked<UMovieSceneShotSection>(Section)->GetCameraGuid(), CameraObjects);

		if (CameraObjects.Num() == 1)
		{
			RuntimeCameraObjects.Add(CameraObjects[0]);
		}
		else
		{
			// No valid camera object was found, take up space.  There should be one entry per section
			RuntimeCameraObjects.Add(nullptr);
		}
	}
}


void FMovieSceneShotTrackInstance::Update(float Position, float LastPosition, const TArray<UObject*>& RuntimeObjects, class IMovieScenePlayer& Player, FMovieSceneSequenceInstance& SequenceInstance) 
{
	const TArray<UMovieSceneSection*>& ShotSections = ShotTrack->GetAllSections();

	for (int32 ShotIndex = 0; ShotIndex < ShotSections.Num(); ++ShotIndex)
	{
		UMovieSceneShotSection* ShotSection = CastChecked<UMovieSceneShotSection>(ShotSections[ShotIndex]);

		// Note shot times are not inclusive
		// @todo Sequencer: This could be faster with a binary search
		if (ShotSection->GetStartTime() <= Position && ShotSection->GetEndTime() > Position)
		{
			UObject* Camera = RuntimeCameraObjects[ShotIndex].Get();
			
			const bool bNewCameraCut = CurrentCameraObject != Camera;
			Player.UpdateCameraCut(Camera, bNewCameraCut);

			CurrentCameraObject = Camera;

			// no need to process any more shots.  Only one shot can be active at a time
			break;
		}
	}
}
