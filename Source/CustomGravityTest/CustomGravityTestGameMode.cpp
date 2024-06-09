// Copyright Epic Games, Inc. All Rights Reserved.

#include "CustomGravityTestGameMode.h"
#include "CustomGravityTestCharacter.h"
#include "UObject/ConstructorHelpers.h"

ACustomGravityTestGameMode::ACustomGravityTestGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
