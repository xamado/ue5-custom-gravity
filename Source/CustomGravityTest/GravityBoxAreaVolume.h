// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "GravityBoxAreaVolume.generated.h"


UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class CUSTOMGRAVITYTEST_API UGravityBoxAreaVolume : public UBoxComponent
{
	GENERATED_BODY()

public:
	UGravityBoxAreaVolume();

protected:
	virtual void BeginPlay() override;
};
