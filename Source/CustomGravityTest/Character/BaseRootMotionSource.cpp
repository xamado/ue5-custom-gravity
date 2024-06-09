// Copyright Epic Games, Inc. All Rights Reserved.

#include "BaseRootMotionSource.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "BaseCharacter.h"
#include "EngineLogs.h"
#include "BaseCharacterMovementComponent.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveFloat.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BaseRootMotionSource)

#if ROOT_MOTION_DEBUG
TAutoConsoleVariable<int32> BaseRootMotionSourceDebug::CVarDebugRootMotionSources(
	TEXT("p.RootMotion.Debug"),
	0,
	TEXT("Whether to draw root motion source debug information.\n")
	TEXT("0: Disable, 1: Enable"),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarDebugRootMotionSourcesLifetime(
	TEXT("p.RootMotion.DebugSourceLifeTime"),
	6.f,
	TEXT("How long a visualized root motion source persists.\n")
	TEXT("Time in seconds each visualized root motion source persists."),
	ECVF_Cheat);

void BaseRootMotionSourceDebug::PrintOnScreen(const ABaseCharacter& InCharacter, const FString& InString)
{
	// Skip bots, debug player networking.
	if (InCharacter.IsPlayerControlled())
	{
		const FString AdjustedDebugString = FString::Printf(TEXT("[%d] [%s] %s"), (uint64)GFrameCounter, *InCharacter.GetName(), *InString);

		// If on the server, replicate this message to everyone.
		if (!InCharacter.IsLocallyControlled() && (InCharacter.GetLocalRole() == ROLE_Authority))
		{
			for (FConstPlayerControllerIterator Iterator = InCharacter.GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator)
			{
				if (const APlayerController* const PlayerController = Iterator->Get())
				{
					if (ABaseCharacter* const Character = Cast<ABaseCharacter>(PlayerController->GetPawn()))
					{
						Character->RootMotionDebugClientPrintOnScreen(AdjustedDebugString);
					}
				}
			}
		}
		else
		{
			const FColor DebugColor = (InCharacter.IsLocallyControlled()) ? FColor::Green : FColor::Purple;
			GEngine->AddOnScreenDebugMessage(INDEX_NONE, 0.f, DebugColor, AdjustedDebugString, false, FVector2D::UnitVector * 1.5f);

			UE_LOG(LogRootMotion, Verbose, TEXT("%s"), *AdjustedDebugString);
		}
	}
}

void BaseRootMotionSourceDebug::PrintOnScreenServerMsg(const FString& InString)
{
	const FColor DebugColor = FColor::Red;
	GEngine->AddOnScreenDebugMessage(INDEX_NONE, 0.f, DebugColor, InString, false, FVector2D::UnitVector * 1.5f);

	UE_LOG(LogRootMotion, Verbose, TEXT("%s"), *InString);
}

#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)


const float RootMotionSource_InvalidStartTime = -UE_BIG_NUMBER;


static float EvaluateFloatCurveAtFraction(const UCurveFloat& Curve, const float Fraction)
{
	float MinCurveTime(0.f);
	float MaxCurveTime(1.f);

	Curve.GetTimeRange(MinCurveTime, MaxCurveTime);
	return Curve.GetFloatValue(FMath::GetRangeValue(FVector2f(MinCurveTime, MaxCurveTime), Fraction));
}

static FVector EvaluateVectorCurveAtFraction(const UCurveVector& Curve, const float Fraction)
{
	float MinCurveTime(0.f);
	float MaxCurveTime(1.f);

	Curve.GetTimeRange(MinCurveTime, MaxCurveTime);
	return Curve.GetVectorValue(FMath::GetRangeValue(FVector2f(MinCurveTime, MaxCurveTime), Fraction));
}


//
// FBaseRootMotionServerToLocalIDMapping
//

FBaseRootMotionServerToLocalIDMapping::FBaseRootMotionServerToLocalIDMapping()
	: ServerID(0)
	, LocalID(0)
	, TimeStamp(0.0f)
{
}

bool FBaseRootMotionServerToLocalIDMapping::IsStillValid(float CurrentTimeStamp)
{
	const float MappingValidityDuration = 3.0f; // Mappings updated within this many seconds are still valid
	return TimeStamp >= (CurrentTimeStamp - MappingValidityDuration);
}

//
// FBaseRootMotionSourceStatus
//

FBaseRootMotionSourceStatus::FBaseRootMotionSourceStatus()
	: Flags(0)
{
}

void FBaseRootMotionSourceStatus::Clear()
{
	Flags = 0;
}

void FBaseRootMotionSourceStatus::SetFlag(EBaseRootMotionSourceStatusFlags Flag)
{
	Flags |= (uint8)Flag;
}

void FBaseRootMotionSourceStatus::UnSetFlag(EBaseRootMotionSourceStatusFlags Flag)
{
	Flags &= ~((uint8)Flag);
}

bool FBaseRootMotionSourceStatus::HasFlag(EBaseRootMotionSourceStatusFlags Flag) const
{
	return (Flags & (uint8)Flag) != 0;
}

//
// FBaseRootMotionSourceSettings
//

FBaseRootMotionSourceSettings::FBaseRootMotionSourceSettings()
	: Flags(0)
{
}

void FBaseRootMotionSourceSettings::Clear()
{
	Flags = 0;
}

void FBaseRootMotionSourceSettings::SetFlag(EBaseRootMotionSourceSettingsFlags Flag)
{
	Flags |= (uint8)Flag;
}

void FBaseRootMotionSourceSettings::UnSetFlag(EBaseRootMotionSourceSettingsFlags Flag)
{
	Flags &= ~((uint8)Flag);
}

bool FBaseRootMotionSourceSettings::HasFlag(EBaseRootMotionSourceSettingsFlags Flag) const
{
	return (Flags & (uint8)Flag) != 0;
}

FBaseRootMotionSourceSettings& FBaseRootMotionSourceSettings::operator+=(const FBaseRootMotionSourceSettings& Other)
{
	Flags |= Other.Flags;
	return *this;
}

//
// FBaseRootMotionSource
//

FBaseRootMotionSource::FBaseRootMotionSource()
	: Priority(0)
	, LocalID((uint16)EBaseRootMotionSourceID::Invalid)
	, AccumulateMode(EBaseRootMotionAccumulateMode::Override)
	, StartTime(RootMotionSource_InvalidStartTime)
	, CurrentTime(0.0f)
	, PreviousTime(0.0f)
	, Duration(-1.0f)
	, bInLocalSpace(false)
	, bNeedsSimulatedCatchup(false)
	, bSimulatedNeedsSmoothing(false)
{
}

float FBaseRootMotionSource::GetTime() const
{
	return CurrentTime;
}

float FBaseRootMotionSource::GetStartTime() const
{
	return StartTime;
}

bool FBaseRootMotionSource::IsStartTimeValid() const
{
	return (StartTime != RootMotionSource_InvalidStartTime);
}

float FBaseRootMotionSource::GetDuration() const
{
	return Duration;
}

bool FBaseRootMotionSource::IsTimeOutEnabled() const
{
	return Duration >= 0.f;
}

FBaseRootMotionSource* FBaseRootMotionSource::Clone() const
{
	// If child classes don't override this, savedmoves will not work
	checkf(false, TEXT("FBaseRootMotionSource::Clone() being called erroneously. This should always be overridden in child classes!"));
	return nullptr;
}

bool FBaseRootMotionSource::IsActive() const
{
	return true;
}

bool FBaseRootMotionSource::Matches(const FBaseRootMotionSource* Other) const
{
	return Other != nullptr && 
		GetScriptStruct() == Other->GetScriptStruct() && 
		Priority == Other->Priority &&
		AccumulateMode == Other->AccumulateMode &&
		bInLocalSpace == Other->bInLocalSpace &&
		InstanceName == Other->InstanceName &&
		FMath::IsNearlyEqual(Duration, Other->Duration, UE_SMALL_NUMBER);
}

bool FBaseRootMotionSource::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!Matches(Other))
	{
		return false;
	}

	// Check state
	return Status.Flags == Other->Status.Flags && GetTime() == Other->GetTime();
}

bool FBaseRootMotionSource::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (SourceToTakeStateFrom != nullptr)
	{
		if (GetScriptStruct() == SourceToTakeStateFrom->GetScriptStruct())
		{
			bNeedsSimulatedCatchup = bMarkForSimulatedCatchup;

			const bool bWasMarkedForRemoval = Status.HasFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			Status = SourceToTakeStateFrom->Status;
			// Never undo removal when updating state from another source, should always be guaranteed
			if (bWasMarkedForRemoval)
			{
				Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			}

			SetTime(SourceToTakeStateFrom->GetTime());
			return true;
		}
		else
		{
			// UpdateStateFrom() should only be called on matching Sources. If we hit this case,
			// we have an issue with Matches() and/or LocalIDs being mapped to invalid "partners"
			checkf(false, TEXT("FBaseRootMotionSource::UpdateStateFrom() is being updated from non-matching Source!"));
		}
	}

	return false;
}

void FBaseRootMotionSource::SetTime(float NewTime)
{
	PreviousTime = CurrentTime;
	CurrentTime = NewTime;

	CheckTimeOut();
}

void FBaseRootMotionSource::CheckTimeOut()
{
	// If I'm beyond my duration, I'm finished and can be removed
	if (IsTimeOutEnabled())
	{
		const bool bTimedOut = CurrentTime >= Duration;
		if (bTimedOut)
		{
			Status.SetFlag(EBaseRootMotionSourceStatusFlags::Finished);
		}
		else
		{
			Status.UnSetFlag(EBaseRootMotionSourceStatusFlags::Finished);
		}
	}
}

void FBaseRootMotionSource::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();
}

bool FBaseRootMotionSource::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	Ar << Priority;
	Ar << LocalID;

	uint8 AccumulateModeSerialize = (uint8) AccumulateMode;
	Ar << AccumulateModeSerialize;
	AccumulateMode = (EBaseRootMotionAccumulateMode) AccumulateModeSerialize;

	Ar << InstanceName;
	Ar << CurrentTime;
	Ar << Duration;
	Ar << Status.Flags;
	Ar << bInLocalSpace;
	//Ar << RootMotionParams; // Do we need this for simulated proxies?

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource::GetScriptStruct() const
{
	return FBaseRootMotionSource::StaticStruct();
}

FString FBaseRootMotionSource::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u] FBaseRootMotionSource %s"), LocalID, *InstanceName.GetPlainNameString());
}

//
// FBaseRootMotionSource_ConstantForce
//

FBaseRootMotionSource_ConstantForce::FBaseRootMotionSource_ConstantForce()
	: Force(ForceInitToZero)
	, StrengthOverTime(nullptr)
{
	// Disable Partial End Tick for Constant Forces.
	// Otherwise we end up with very inconsistent velocities on the last frame.
	// This ensures that the ending velocity is maintained and consistent.
	Settings.SetFlag(EBaseRootMotionSourceSettingsFlags::DisablePartialEndTick);
}

FBaseRootMotionSource* FBaseRootMotionSource_ConstantForce::Clone() const
{
	FBaseRootMotionSource_ConstantForce* CopyPtr = new FBaseRootMotionSource_ConstantForce(*this);
	return CopyPtr;
}

bool FBaseRootMotionSource_ConstantForce::Matches(const FBaseRootMotionSource* Other) const
{
	if (!FBaseRootMotionSource::Matches(Other))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_ConstantForce* OtherCast = static_cast<const FBaseRootMotionSource_ConstantForce*>(Other);

	return FVector::PointsAreNear(Force, OtherCast->Force, 0.1f) &&
		StrengthOverTime == OtherCast->StrengthOverTime;
}

bool FBaseRootMotionSource_ConstantForce::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!FBaseRootMotionSource::MatchesAndHasSameState(Other))
	{
		return false;
	}

	return true; // ConstantForce has no unique state
}

bool FBaseRootMotionSource_ConstantForce::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!FBaseRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup))
	{
		return false;
	}

	return true; // ConstantForce has no unique state other than Time which is handled by FBaseRootMotionSource
}

void FBaseRootMotionSource_ConstantForce::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();

	FTransform NewTransform(Force);

	// Scale strength of force over time
	if (StrengthOverTime)
	{
		const float TimeValue = Duration > 0.f ? FMath::Clamp(GetTime() / Duration, 0.f, 1.f) : GetTime();
		const float TimeFactor = StrengthOverTime->GetFloatValue(TimeValue);
		NewTransform.ScaleTranslation(TimeFactor);
	}

	// Scale force based on Simulation/MovementTime differences
	// Ex: Force is to go 200 cm per second forward.
	//     To catch up with server state we need to apply
	//     3 seconds of this root motion in 1 second of
	//     movement tick time -> we apply 600 cm for this frame
	const float Multiplier = (MovementTickTime > UE_SMALL_NUMBER) ? (SimulationTime / MovementTickTime) : 1.f;
	NewTransform.ScaleTranslation(Multiplier);

#if ROOT_MOTION_DEBUG
	if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
	{
		FString AdjustedDebugString = FString::Printf(TEXT("FBaseRootMotionSource_ConstantForce::PrepareRootMotion NewTransform(%s) Multiplier(%f)"),
			*NewTransform.GetTranslation().ToCompactString(), Multiplier);
		BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
	}
#endif

	RootMotionParams.Set(NewTransform);

	SetTime(GetTime() + SimulationTime);
}

bool FBaseRootMotionSource_ConstantForce::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FBaseRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << Force; // TODO-RootMotionSource: Quantization
	Ar << StrengthOverTime;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource_ConstantForce::GetScriptStruct() const
{
	return FBaseRootMotionSource_ConstantForce::StaticStruct();
}

FString FBaseRootMotionSource_ConstantForce::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FBaseRootMotionSource_ConstantForce %s"), LocalID, *InstanceName.GetPlainNameString());
}

void FBaseRootMotionSource_ConstantForce::AddReferencedObjects(class FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(StrengthOverTime);

	FBaseRootMotionSource::AddReferencedObjects(Collector);
}

//
// FBaseRootMotionSource_RadialForce
//

FBaseRootMotionSource_RadialForce::FBaseRootMotionSource_RadialForce()
	: Location(ForceInitToZero)
	, LocationActor(nullptr)
	, Radius(1.f)
	, Strength(0.f)
	, bIsPush(true)
	, bNoZForce(false)
	, StrengthDistanceFalloff(nullptr)
	, StrengthOverTime(nullptr)
	, bUseFixedWorldDirection(false)
	, FixedWorldDirection(ForceInitToZero)
{
}

FBaseRootMotionSource* FBaseRootMotionSource_RadialForce::Clone() const
{
	FBaseRootMotionSource_RadialForce* CopyPtr = new FBaseRootMotionSource_RadialForce(*this);
	return CopyPtr;
}

bool FBaseRootMotionSource_RadialForce::Matches(const FBaseRootMotionSource* Other) const
{
	if (!FBaseRootMotionSource::Matches(Other))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_RadialForce* OtherCast = static_cast<const FBaseRootMotionSource_RadialForce*>(Other);

	return bIsPush == OtherCast->bIsPush &&
		bNoZForce == OtherCast->bNoZForce &&
		bUseFixedWorldDirection == OtherCast->bUseFixedWorldDirection &&
		StrengthDistanceFalloff == OtherCast->StrengthDistanceFalloff &&
		StrengthOverTime == OtherCast->StrengthOverTime &&
		(LocationActor == OtherCast->LocationActor ||
		FVector::PointsAreNear(Location, OtherCast->Location, 1.0f)) &&
		FMath::IsNearlyEqual(Radius, OtherCast->Radius, UE_SMALL_NUMBER) &&
		FMath::IsNearlyEqual(Strength, OtherCast->Strength, UE_SMALL_NUMBER) &&
		FixedWorldDirection.Equals(OtherCast->FixedWorldDirection, 3.0f);
}

bool FBaseRootMotionSource_RadialForce::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!FBaseRootMotionSource::MatchesAndHasSameState(Other))
	{
		return false;
	}

	return true; // RadialForce has no unique state
}

bool FBaseRootMotionSource_RadialForce::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!FBaseRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup))
	{
		return false;
	}

	return true; // RadialForce has no unique state other than Time which is handled by FBaseRootMotionSource
}

void FBaseRootMotionSource_RadialForce::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();

	const FVector CharacterLocation = Character.GetActorLocation();
	FVector Force = FVector::ZeroVector;
	const FVector ForceLocation = LocationActor ? LocationActor->GetActorLocation() : Location;
	float Distance = FVector::Dist(ForceLocation, CharacterLocation);
	if (Distance < Radius)
	{
		// Calculate strength
		float CurrentStrength = Strength;
		{
			float AdditiveStrengthFactor = 1.f;
			if (StrengthDistanceFalloff)
			{
				const float DistanceFactor = StrengthDistanceFalloff->GetFloatValue(FMath::Clamp(Distance / Radius, 0.f, 1.f));
				AdditiveStrengthFactor -= (1.f - DistanceFactor);
			}

			if (StrengthOverTime)
			{
				const float TimeValue = Duration > 0.f ? FMath::Clamp(GetTime() / Duration, 0.f, 1.f) : GetTime();
				const float TimeFactor = StrengthOverTime->GetFloatValue(TimeValue);
				AdditiveStrengthFactor -= (1.f - TimeFactor);
			}

			CurrentStrength = Strength * FMath::Clamp(AdditiveStrengthFactor, 0.f, 1.f);
		}

		if (bUseFixedWorldDirection)
		{
			Force = FixedWorldDirection.Vector() * CurrentStrength;
		}
		else
		{
			Force = (ForceLocation - CharacterLocation).GetSafeNormal() * CurrentStrength;
			
			if (bIsPush)
			{
				Force *= -1.f;
			}
		}
	}

	if (bNoZForce)
	{
		Force.Z = 0.f;
	}

	FTransform NewTransform(Force);

	// Scale force based on Simulation/MovementTime differences
	// Ex: Force is to go 200 cm per second forward.
	//     To catch up with server state we need to apply
	//     3 seconds of this root motion in 1 second of
	//     movement tick time -> we apply 600 cm for this frame
	if (SimulationTime != MovementTickTime && MovementTickTime > UE_SMALL_NUMBER)
	{
		const float Multiplier = SimulationTime / MovementTickTime;
		NewTransform.ScaleTranslation(Multiplier);
	}

	RootMotionParams.Set(NewTransform);

	SetTime(GetTime() + SimulationTime);
}

bool FBaseRootMotionSource_RadialForce::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FBaseRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << Location; // TODO-RootMotionSource: Quantization
	Ar << LocationActor;
	Ar << Radius;
	Ar << Strength;
	Ar << bIsPush;
	Ar << bNoZForce;
	Ar << StrengthDistanceFalloff;
	Ar << StrengthOverTime;
	Ar << bUseFixedWorldDirection;
	Ar << FixedWorldDirection;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource_RadialForce::GetScriptStruct() const
{
	return FBaseRootMotionSource_RadialForce::StaticStruct();
}

FString FBaseRootMotionSource_RadialForce::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FBaseRootMotionSource_RadialForce %s"), LocalID, *InstanceName.GetPlainNameString());
}

void FBaseRootMotionSource_RadialForce::AddReferencedObjects(class FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(LocationActor);
	Collector.AddReferencedObject(StrengthDistanceFalloff);
	Collector.AddReferencedObject(StrengthOverTime);

	FBaseRootMotionSource::AddReferencedObjects(Collector);
}

//
// FBaseRootMotionSource_MoveToForce
//

FBaseRootMotionSource_MoveToForce::FBaseRootMotionSource_MoveToForce()
	: StartLocation(ForceInitToZero)
	, TargetLocation(ForceInitToZero)
	, bRestrictSpeedToExpected(false)
	, PathOffsetCurve(nullptr)
{
}

FBaseRootMotionSource* FBaseRootMotionSource_MoveToForce::Clone() const
{
	FBaseRootMotionSource_MoveToForce* CopyPtr = new FBaseRootMotionSource_MoveToForce(*this);
	return CopyPtr;
}

bool FBaseRootMotionSource_MoveToForce::Matches(const FBaseRootMotionSource* Other) const
{
	if (!FBaseRootMotionSource::Matches(Other))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_MoveToForce* OtherCast = static_cast<const FBaseRootMotionSource_MoveToForce*>(Other);

	return bRestrictSpeedToExpected == OtherCast->bRestrictSpeedToExpected &&
		PathOffsetCurve == OtherCast->PathOffsetCurve &&
		FVector::PointsAreNear(TargetLocation, OtherCast->TargetLocation, 0.1f);
}

bool FBaseRootMotionSource_MoveToForce::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!FBaseRootMotionSource::MatchesAndHasSameState(Other))
	{
		return false;
	}

	return true; // MoveToForce has no unique state
}

bool FBaseRootMotionSource_MoveToForce::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!FBaseRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup))
	{
		return false;
	}

	return true; // MoveToForce has no unique state other than Time which is handled by FBaseRootMotionSource
}

void FBaseRootMotionSource_MoveToForce::SetTime(float NewTime)
{
	FBaseRootMotionSource::SetTime(NewTime);

	// TODO-RootMotionSource: Check if reached destination?
}

FVector FBaseRootMotionSource_MoveToForce::GetPathOffsetInWorldSpace(const float MoveFraction) const
{
	if (PathOffsetCurve)
	{
		// Calculate path offset
		const FVector PathOffsetInFacingSpace = EvaluateVectorCurveAtFraction(*PathOffsetCurve, MoveFraction);
		FRotator FacingRotation((TargetLocation-StartLocation).Rotation());
		FacingRotation.Pitch = 0.f; // By default we don't include pitch in the offset, but an option could be added if necessary
		return FacingRotation.RotateVector(PathOffsetInFacingSpace);
	}

	return FVector::ZeroVector;
}

void FBaseRootMotionSource_MoveToForce::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();

	if (Duration > UE_SMALL_NUMBER && MovementTickTime > UE_SMALL_NUMBER)
	{
		const float MoveFraction = (GetTime() + SimulationTime) / Duration;

		FVector CurrentTargetLocation = FMath::Lerp<FVector, float>(StartLocation, TargetLocation, MoveFraction);
		CurrentTargetLocation += GetPathOffsetInWorldSpace(MoveFraction);

		const FVector CurrentLocation = Character.GetActorLocation();

		FVector Force = (CurrentTargetLocation - CurrentLocation) / MovementTickTime;

		if (bRestrictSpeedToExpected && !Force.IsNearlyZero(UE_KINDA_SMALL_NUMBER))
		{
			// Calculate expected current location (if we didn't have collision and moved exactly where our velocity should have taken us)
			const float PreviousMoveFraction = GetTime() / Duration;
			FVector CurrentExpectedLocation = FMath::Lerp<FVector, float>(StartLocation, TargetLocation, PreviousMoveFraction);
			CurrentExpectedLocation += GetPathOffsetInWorldSpace(PreviousMoveFraction);

			// Restrict speed to the expected speed, allowing some small amount of error
			const FVector ExpectedForce = (CurrentTargetLocation - CurrentExpectedLocation) / MovementTickTime;
			const float ExpectedSpeed = ExpectedForce.Size();
			const float CurrentSpeedSqr = Force.SizeSquared();

			const float ErrorAllowance = 0.5f; // in cm/s
			if (CurrentSpeedSqr > FMath::Square(ExpectedSpeed + ErrorAllowance))
			{
				Force.Normalize();
				Force *= ExpectedSpeed;
			}
		}

		// Debug
#if ROOT_MOTION_DEBUG
		if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnGameThread() != 0)
		{
			const FVector LocDiff = MoveComponent.UpdatedComponent->GetComponentLocation() - CurrentLocation;
			const float DebugLifetime = CVarDebugRootMotionSourcesLifetime.GetValueOnGameThread();

			// Current
			DrawDebugCapsule(Character.GetWorld(), MoveComponent.UpdatedComponent->GetComponentLocation(), Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Red, true, DebugLifetime);

			// Current Target
			DrawDebugCapsule(Character.GetWorld(), CurrentTargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Green, true, DebugLifetime);

			// Target
			DrawDebugCapsule(Character.GetWorld(), TargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Blue, true, DebugLifetime);

			// Force
			DrawDebugLine(Character.GetWorld(), CurrentLocation, CurrentLocation+Force, FColor::Blue, true, DebugLifetime);
		}
#endif

		FTransform NewTransform(Force);
		RootMotionParams.Set(NewTransform);
	}
	else
	{
		checkf(Duration > UE_SMALL_NUMBER, TEXT("FBaseRootMotionSource_MoveToForce prepared with invalid duration."));
	}

	SetTime(GetTime() + SimulationTime);
}

bool FBaseRootMotionSource_MoveToForce::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FBaseRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << StartLocation; // TODO-RootMotionSource: Quantization
	Ar << TargetLocation; // TODO-RootMotionSource: Quantization
	Ar << bRestrictSpeedToExpected;
	Ar << PathOffsetCurve;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource_MoveToForce::GetScriptStruct() const
{
	return FBaseRootMotionSource_MoveToForce::StaticStruct();
}

FString FBaseRootMotionSource_MoveToForce::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FBaseRootMotionSource_MoveToForce %s"), LocalID, *InstanceName.GetPlainNameString());
}

void FBaseRootMotionSource_MoveToForce::AddReferencedObjects(class FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PathOffsetCurve);

	FBaseRootMotionSource::AddReferencedObjects(Collector);
}

//
// FBaseRootMotionSource_MoveToDynamicForce
//

FBaseRootMotionSource_MoveToDynamicForce::FBaseRootMotionSource_MoveToDynamicForce()
	: StartLocation(ForceInitToZero)
	, InitialTargetLocation(ForceInitToZero)
	, TargetLocation(ForceInitToZero)
	, bRestrictSpeedToExpected(false)
	, PathOffsetCurve(nullptr)
	, TimeMappingCurve(nullptr)
{
}

void FBaseRootMotionSource_MoveToDynamicForce::SetTargetLocation(FVector NewTargetLocation)
{
	TargetLocation = NewTargetLocation;
}

FBaseRootMotionSource* FBaseRootMotionSource_MoveToDynamicForce::Clone() const
{
	FBaseRootMotionSource_MoveToDynamicForce* CopyPtr = new FBaseRootMotionSource_MoveToDynamicForce(*this);
	return CopyPtr;
}

bool FBaseRootMotionSource_MoveToDynamicForce::Matches(const FBaseRootMotionSource* Other) const
{
	if (!FBaseRootMotionSource::Matches(Other))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_MoveToDynamicForce* OtherCast = static_cast<const FBaseRootMotionSource_MoveToDynamicForce*>(Other);

	return bRestrictSpeedToExpected == OtherCast->bRestrictSpeedToExpected &&
		PathOffsetCurve == OtherCast->PathOffsetCurve &&
		TimeMappingCurve == OtherCast->TimeMappingCurve;
}

bool FBaseRootMotionSource_MoveToDynamicForce::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!FBaseRootMotionSource::MatchesAndHasSameState(Other))
	{
		return false;
	}
	
	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_MoveToDynamicForce* OtherCast = static_cast<const FBaseRootMotionSource_MoveToDynamicForce*>(Other);

	return (StartLocation.Equals(OtherCast->StartLocation) &&
			TargetLocation.Equals(OtherCast->TargetLocation));
}

bool FBaseRootMotionSource_MoveToDynamicForce::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!FBaseRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::UpdateStateFrom() we ensured ScriptStruct equality
	const FBaseRootMotionSource_MoveToDynamicForce* OtherCast = static_cast<const FBaseRootMotionSource_MoveToDynamicForce*>(SourceToTakeStateFrom);

	StartLocation = OtherCast->StartLocation;
	TargetLocation = OtherCast->TargetLocation;

	return true;
}

void FBaseRootMotionSource_MoveToDynamicForce::SetTime(float NewTime)
{
	FBaseRootMotionSource::SetTime(NewTime);

	// TODO-RootMotionSource: Check if reached destination?
}

FVector FBaseRootMotionSource_MoveToDynamicForce::GetPathOffsetInWorldSpace(const float MoveFraction) const
{
	if (PathOffsetCurve)
	{
		// Calculate path offset
		const FVector PathOffsetInFacingSpace = EvaluateVectorCurveAtFraction(*PathOffsetCurve, MoveFraction);
		FRotator FacingRotation((TargetLocation-StartLocation).Rotation());
		FacingRotation.Pitch = 0.f; // By default we don't include pitch in the offset, but an option could be added if necessary
		return FacingRotation.RotateVector(PathOffsetInFacingSpace);
	}

	return FVector::ZeroVector;
}

void FBaseRootMotionSource_MoveToDynamicForce::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();

	if (Duration > UE_SMALL_NUMBER && MovementTickTime > UE_SMALL_NUMBER)
	{
		float MoveFraction = (GetTime() + SimulationTime) / Duration;
		
		if (TimeMappingCurve)
		{
			MoveFraction = EvaluateFloatCurveAtFraction(*TimeMappingCurve, MoveFraction);
		}

		FVector CurrentTargetLocation = FMath::Lerp<FVector, float>(StartLocation, TargetLocation, MoveFraction);
		CurrentTargetLocation += GetPathOffsetInWorldSpace(MoveFraction);

		const FVector CurrentLocation = Character.GetActorLocation();

		FVector Force = (CurrentTargetLocation - CurrentLocation) / MovementTickTime;

		if (bRestrictSpeedToExpected && !Force.IsNearlyZero(UE_KINDA_SMALL_NUMBER))
		{
			// Calculate expected current location (if we didn't have collision and moved exactly where our velocity should have taken us)
			float PreviousMoveFraction = GetTime() / Duration;
			if (TimeMappingCurve)
			{
				PreviousMoveFraction = EvaluateFloatCurveAtFraction(*TimeMappingCurve, PreviousMoveFraction);
			}

			FVector CurrentExpectedLocation = FMath::Lerp<FVector, float>(StartLocation, TargetLocation, PreviousMoveFraction);
			CurrentExpectedLocation += GetPathOffsetInWorldSpace(PreviousMoveFraction);

			// Restrict speed to the expected speed, allowing some small amount of error
			const FVector ExpectedForce = (CurrentTargetLocation - CurrentExpectedLocation) / MovementTickTime;
			const float ExpectedSpeed = ExpectedForce.Size();
			const float CurrentSpeedSqr = Force.SizeSquared();

			const float ErrorAllowance = 0.5f; // in cm/s
			if (CurrentSpeedSqr > FMath::Square(ExpectedSpeed + ErrorAllowance))
			{
				Force.Normalize();
				Force *= ExpectedSpeed;
			}
		}

		// Debug
#if ROOT_MOTION_DEBUG
		if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnGameThread() != 0)
		{
			const FVector LocDiff = MoveComponent.UpdatedComponent->GetComponentLocation() - CurrentLocation;
			const float DebugLifetime = CVarDebugRootMotionSourcesLifetime.GetValueOnGameThread();

			// Current
			DrawDebugCapsule(Character.GetWorld(), MoveComponent.UpdatedComponent->GetComponentLocation(), Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Red, true, DebugLifetime);

			// Current Target
			DrawDebugCapsule(Character.GetWorld(), CurrentTargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Green, true, DebugLifetime);

			// Target
			DrawDebugCapsule(Character.GetWorld(), TargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Blue, true, DebugLifetime);

			// Force
			DrawDebugLine(Character.GetWorld(), CurrentLocation, CurrentLocation+Force, FColor::Blue, true, DebugLifetime);
		}
#endif

		FTransform NewTransform(Force);
		RootMotionParams.Set(NewTransform);
	}
	else
	{
		checkf(Duration > UE_SMALL_NUMBER, TEXT("FBaseRootMotionSource_MoveToDynamicForce prepared with invalid duration."));
	}

	SetTime(GetTime() + SimulationTime);
}

bool FBaseRootMotionSource_MoveToDynamicForce::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FBaseRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << StartLocation; // TODO-RootMotionSource: Quantization
	Ar << InitialTargetLocation; // TODO-RootMotionSource: Quantization
	Ar << TargetLocation; // TODO-RootMotionSource: Quantization
	Ar << bRestrictSpeedToExpected;
	Ar << PathOffsetCurve;
	Ar << TimeMappingCurve;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource_MoveToDynamicForce::GetScriptStruct() const
{
	return FBaseRootMotionSource_MoveToDynamicForce::StaticStruct();
}

FString FBaseRootMotionSource_MoveToDynamicForce::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FBaseRootMotionSource_MoveToDynamicForce %s"), LocalID, *InstanceName.GetPlainNameString());
}

void FBaseRootMotionSource_MoveToDynamicForce::AddReferencedObjects(class FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PathOffsetCurve);
	Collector.AddReferencedObject(TimeMappingCurve);

	FBaseRootMotionSource::AddReferencedObjects(Collector);
}


//
// FBaseRootMotionSource_JumpForce
//

FBaseRootMotionSource_JumpForce::FBaseRootMotionSource_JumpForce()
	: Rotation(ForceInitToZero)
	, Distance(-1.0f)
	, Height(-1.0f)
	, bDisableTimeout(false)
	, PathOffsetCurve(nullptr)
	, TimeMappingCurve(nullptr)
	, SavedHalfwayLocation(FVector::ZeroVector)
{
	// Don't allow partial end ticks. Jump forces are meant to provide velocity that
	// carries through to the end of the jump, and if we do partial ticks at the very end,
	// it means the provided velocity can be significantly reduced on the very last tick,
	// resulting in lost momentum. This is not desirable for jumps.
	Settings.SetFlag(EBaseRootMotionSourceSettingsFlags::DisablePartialEndTick);
}

bool FBaseRootMotionSource_JumpForce::IsTimeOutEnabled() const
{
	if (bDisableTimeout)
	{
		return false;
	}
	return FBaseRootMotionSource::IsTimeOutEnabled();
}

FBaseRootMotionSource* FBaseRootMotionSource_JumpForce::Clone() const
{
	FBaseRootMotionSource_JumpForce* CopyPtr = new FBaseRootMotionSource_JumpForce(*this);
	return CopyPtr;
}

bool FBaseRootMotionSource_JumpForce::Matches(const FBaseRootMotionSource* Other) const
{
	if (!FBaseRootMotionSource::Matches(Other))
	{
		return false;
	}

	// We can cast safely here since in FBaseRootMotionSource::Matches() we ensured ScriptStruct equality
	const FBaseRootMotionSource_JumpForce* OtherCast = static_cast<const FBaseRootMotionSource_JumpForce*>(Other);

	return bDisableTimeout == OtherCast->bDisableTimeout &&
		PathOffsetCurve == OtherCast->PathOffsetCurve &&
		TimeMappingCurve == OtherCast->TimeMappingCurve &&
		FMath::IsNearlyEqual(Distance, OtherCast->Distance, UE_SMALL_NUMBER) &&
		FMath::IsNearlyEqual(Height, OtherCast->Height, UE_SMALL_NUMBER) &&
		Rotation.Equals(OtherCast->Rotation, 1.0f);
}

bool FBaseRootMotionSource_JumpForce::MatchesAndHasSameState(const FBaseRootMotionSource* Other) const
{
	// Check that it matches
	if (!FBaseRootMotionSource::MatchesAndHasSameState(Other))
	{
		return false;
	}

	return true; // JumpForce has no unique state
}

bool FBaseRootMotionSource_JumpForce::UpdateStateFrom(const FBaseRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!FBaseRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup))
	{
		return false;
	}

	return true; // JumpForce has no unique state other than Time which is handled by FBaseRootMotionSource
}

FVector FBaseRootMotionSource_JumpForce::GetPathOffset(const float MoveFraction) const
{
	FVector PathOffset(FVector::ZeroVector);
	if (PathOffsetCurve)
	{
		// Calculate path offset
		PathOffset = EvaluateVectorCurveAtFraction(*PathOffsetCurve, MoveFraction);
	}
	else
	{
		// Default to "jump parabola", a simple x^2 shifted to be upside-down and shifted
		// to get [0,1] X (MoveFraction/Distance) mapping to [0,1] Y (height)
		// Height = -(2x-1)^2 + 1
		const float Phi = 2.f*MoveFraction - 1;
		const float Z = -(Phi*Phi) + 1;
		PathOffset.Z = Z;
	}

	// Scale Z offset to height. If Height < 0, we use direct path offset values
	if (Height >= 0.f)
	{
		PathOffset.Z *= Height;
	}

	return PathOffset;
}

FVector FBaseRootMotionSource_JumpForce::GetRelativeLocation(float MoveFraction) const
{
	// Given MoveFraction, what relative location should a character be at?
	FRotator FacingRotation(Rotation);
	FacingRotation.Pitch = 0.f; // By default we don't include pitch, but an option could be added if necessary

	FVector RelativeLocationFacingSpace = FVector(MoveFraction * Distance, 0.f, 0.f) + GetPathOffset(MoveFraction);

	return FacingRotation.RotateVector(RelativeLocationFacingSpace);
}

void FBaseRootMotionSource_JumpForce::PrepareRootMotion
	(
		float SimulationTime, 
		float MovementTickTime,
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent
	)
{
	RootMotionParams.Clear();

	if (Duration > UE_SMALL_NUMBER && MovementTickTime > UE_SMALL_NUMBER && SimulationTime > UE_SMALL_NUMBER)
	{
		float CurrentTimeFraction = GetTime() / Duration;
		float TargetTimeFraction = (GetTime() + SimulationTime) / Duration;

		// If we're beyond specified duration, we need to re-map times so that
		// we continue our desired ending velocity
		if (TargetTimeFraction > 1.f)
		{
			float TimeFractionPastAllowable = TargetTimeFraction - 1.0f;
			TargetTimeFraction -= TimeFractionPastAllowable;
			CurrentTimeFraction -= TimeFractionPastAllowable;
		}

		float CurrentMoveFraction = CurrentTimeFraction;
		float TargetMoveFraction = TargetTimeFraction;

		if (TimeMappingCurve)
		{
			CurrentMoveFraction = EvaluateFloatCurveAtFraction(*TimeMappingCurve, CurrentMoveFraction);
			TargetMoveFraction  = EvaluateFloatCurveAtFraction(*TimeMappingCurve, TargetMoveFraction);
		}

		const FVector CurrentRelativeLocation = GetRelativeLocation(CurrentMoveFraction);
		const FVector TargetRelativeLocation = GetRelativeLocation(TargetMoveFraction);

		const FVector Force = (TargetRelativeLocation - CurrentRelativeLocation) / MovementTickTime;

		// Debug
#if ROOT_MOTION_DEBUG
		if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnGameThread() != 0)
		{
			const FVector CurrentLocation = Character.GetActorLocation();
			const FVector CurrentTargetLocation = CurrentLocation + (TargetRelativeLocation - CurrentRelativeLocation);
			const FVector LocDiff = MoveComponent.UpdatedComponent->GetComponentLocation() - CurrentLocation;
			const float DebugLifetime = CVarDebugRootMotionSourcesLifetime.GetValueOnGameThread();

			// Current
			DrawDebugCapsule(Character.GetWorld(), MoveComponent.UpdatedComponent->GetComponentLocation(), Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Red, true, DebugLifetime);

			// Current Target
			DrawDebugCapsule(Character.GetWorld(), CurrentTargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Green, true, DebugLifetime);

			// Target
			DrawDebugCapsule(Character.GetWorld(), CurrentTargetLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::Blue, true, DebugLifetime);

			// Force
			DrawDebugLine(Character.GetWorld(), CurrentLocation, CurrentLocation+Force, FColor::Blue, true, DebugLifetime);

			// Halfway point
			const FVector HalfwayLocation = CurrentLocation + (GetRelativeLocation(0.5f) - CurrentRelativeLocation);
			if (SavedHalfwayLocation.IsNearlyZero())
			{
				SavedHalfwayLocation = HalfwayLocation;
			}
			if (FVector::DistSquared(SavedHalfwayLocation, HalfwayLocation) > 50.f*50.f)
			{
				UE_LOG(LogRootMotion, Verbose, TEXT("RootMotion JumpForce drifted from saved halfway calculation!"));
				SavedHalfwayLocation = HalfwayLocation;
			}
			DrawDebugCapsule(Character.GetWorld(), HalfwayLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::White, true, DebugLifetime);

			// Destination point
			const FVector DestinationLocation = CurrentLocation + (GetRelativeLocation(1.0f) - CurrentRelativeLocation);
			DrawDebugCapsule(Character.GetWorld(), DestinationLocation + LocDiff, Character.GetSimpleCollisionHalfHeight(), Character.GetSimpleCollisionRadius(), FQuat::Identity, FColor::White, true, DebugLifetime);

			UE_LOG(LogRootMotion, VeryVerbose, TEXT("RootMotionJumpForce %s %s preparing from %f to %f from (%s) to (%s) resulting force %s"), 
				Character.GetLocalRole() == ROLE_AutonomousProxy ? TEXT("AUTONOMOUS") : TEXT("AUTHORITY"),
				Character.bClientUpdating ? TEXT("UPD") : TEXT("NOR"),
				GetTime(), GetTime() + SimulationTime, 
				*CurrentLocation.ToString(), *CurrentTargetLocation.ToString(), 
				*Force.ToString());

			{
				FString AdjustedDebugString = FString::Printf(TEXT("    FBaseRootMotionSource_JumpForce::Prep Force(%s) SimTime(%.3f) MoveTime(%.3f) StartP(%.3f) EndP(%.3f)"),
					*Force.ToCompactString(), SimulationTime, MovementTickTime, CurrentMoveFraction, TargetMoveFraction);
				BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
			}
		}
#endif

		const FTransform NewTransform(Force);
		RootMotionParams.Set(NewTransform);
	}
	else
	{
		checkf(Duration > UE_SMALL_NUMBER, TEXT("FBaseRootMotionSource_JumpForce prepared with invalid duration."));
	}

	SetTime(GetTime() + SimulationTime);
}

bool FBaseRootMotionSource_JumpForce::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FBaseRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << Rotation; // TODO-RootMotionSource: Quantization
	Ar << Distance;
	Ar << Height;
	Ar << bDisableTimeout;
	Ar << PathOffsetCurve;
	Ar << TimeMappingCurve;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FBaseRootMotionSource_JumpForce::GetScriptStruct() const
{
	return FBaseRootMotionSource_JumpForce::StaticStruct();
}

FString FBaseRootMotionSource_JumpForce::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FBaseRootMotionSource_JumpForce %s"), LocalID, *InstanceName.GetPlainNameString());
}

void FBaseRootMotionSource_JumpForce::AddReferencedObjects(class FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PathOffsetCurve);
	Collector.AddReferencedObject(TimeMappingCurve);

	FBaseRootMotionSource::AddReferencedObjects(Collector);
}

//
// FBaseRootMotionSourceGroup
//

FBaseRootMotionSourceGroup::FBaseRootMotionSourceGroup()
	: bHasAdditiveSources(false)
	, bHasOverrideSources(false)
	, bHasOverrideSourcesWithIgnoreZAccumulate(false)
	, bIsAdditiveVelocityApplied(false)
	, LastPreAdditiveVelocity(ForceInitToZero)
{
}

bool FBaseRootMotionSourceGroup::HasActiveRootMotionSources() const
{
	return RootMotionSources.Num() > 0 || PendingAddRootMotionSources.Num() > 0;
}

bool FBaseRootMotionSourceGroup::HasOverrideVelocity() const
{
	return bHasOverrideSources;
}

bool FBaseRootMotionSourceGroup::HasOverrideVelocityWithIgnoreZAccumulate() const
{
	return bHasOverrideSourcesWithIgnoreZAccumulate;
}

bool FBaseRootMotionSourceGroup::HasAdditiveVelocity() const
{
	return bHasAdditiveSources;
}

bool FBaseRootMotionSourceGroup::HasVelocity() const
{
	return HasOverrideVelocity() || HasAdditiveVelocity();
}

bool FBaseRootMotionSourceGroup::HasRootMotionToApply() const
{
	return HasActiveRootMotionSources();
}

void FBaseRootMotionSourceGroup::CleanUpInvalidRootMotion(float DeltaTime, const ABaseCharacter& Character, UBaseCharacterMovementComponent& MoveComponent)
{
	// Remove active sources marked for removal or that are invalid
	RootMotionSources.RemoveAll([this, DeltaTime, &Character, &MoveComponent](const TSharedPtr<FBaseRootMotionSource>& RootSource)
	{
		if (RootSource.IsValid())
		{
			if (!RootSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval) &&
				!RootSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::Finished))
			{
				return false;
			}

			// When additive root motion sources are removed we add their effects back to Velocity
			// so that any maintained momentum/velocity that they were contributing affects character
			// velocity and it's not a sudden stop
			if (RootSource->AccumulateMode == EBaseRootMotionAccumulateMode::Additive)
			{
				if (bIsAdditiveVelocityApplied)
				{
					const FVector PreviousLastPreAdditiveVelocity = LastPreAdditiveVelocity;
					AccumulateRootMotionVelocityFromSource(*RootSource, DeltaTime, Character, MoveComponent, LastPreAdditiveVelocity);

#if ROOT_MOTION_DEBUG
					if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
					{
						FString AdjustedDebugString = FString::Printf(TEXT("PrepareRootMotion RemovingAdditiveSource LastPreAdditiveVelocity(%s) Old(%s)"),
							*LastPreAdditiveVelocity.ToCompactString(), *PreviousLastPreAdditiveVelocity.ToCompactString());
						BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
					}
#endif
				}
			}

			// Process FinishVelocity options when RootMotionSource is removed.
			if (RootSource->FinishVelocityParams.Mode == EBaseRootMotionFinishVelocityMode::ClampVelocity)
			{
				// For Z, only clamp positive values to prevent shooting off, we don't want to slow down a fall.
				MoveComponent.Velocity = MoveComponent.Velocity.GetClampedToMaxSize2D(RootSource->FinishVelocityParams.ClampVelocity);
				MoveComponent.Velocity.Z = FMath::Min<FVector::FReal>(MoveComponent.Velocity.Z, RootSource->FinishVelocityParams.ClampVelocity);

				// if we have additive velocity applied, LastPreAdditiveVelocity will stomp velocity, so make sure it gets clamped too.
				if (bIsAdditiveVelocityApplied)
				{
					// For Z, only clamp positive values to prevent shooting off, we don't want to slow down a fall.
					LastPreAdditiveVelocity = LastPreAdditiveVelocity.GetClampedToMaxSize2D(RootSource->FinishVelocityParams.ClampVelocity);
					LastPreAdditiveVelocity.Z = FMath::Min<FVector::FReal>(LastPreAdditiveVelocity.Z, RootSource->FinishVelocityParams.ClampVelocity);
				}
			}
			else if (RootSource->FinishVelocityParams.Mode == EBaseRootMotionFinishVelocityMode::SetVelocity)
			{
				MoveComponent.Velocity = RootSource->FinishVelocityParams.SetVelocity;
				// if we have additive velocity applied, LastPreAdditiveVelocity will stomp velocity, so make sure this gets set too.
				if (bIsAdditiveVelocityApplied)
				{
					LastPreAdditiveVelocity = RootSource->FinishVelocityParams.SetVelocity;
				}
			}

			UE_LOG(LogRootMotion, VeryVerbose, TEXT("RootMotionSource being removed: %s"), *RootSource->ToSimpleString());

#if ROOT_MOTION_DEBUG
			if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
			{
				FString AdjustedDebugString = FString::Printf(TEXT("PrepareRootMotion Removing RootMotionSource(%s)"),
					*RootSource->ToSimpleString());
				BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
			}
#endif
		}
		return true;
	});

	// Remove pending sources that could have been marked for removal before they were made active
	PendingAddRootMotionSources.RemoveAll([&Character](const TSharedPtr<FBaseRootMotionSource>& RootSource)
	{
		if (RootSource.IsValid())
		{
			if (!RootSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval) &&
				!RootSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::Finished))
			{
				return false;
			}

			UE_LOG(LogRootMotion, VeryVerbose, TEXT("Pending RootMotionSource being removed: %s"), *RootSource->ToSimpleString());

#if ROOT_MOTION_DEBUG
			if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
			{
				FString AdjustedDebugString = FString::Printf(TEXT("PrepareRootMotion Removing PendingAddRootMotionSource(%s)"),
					*RootSource->ToSimpleString());
				BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
			}
#endif
		}
		return true;
	});
}

void FBaseRootMotionSourceGroup::PrepareRootMotion(float DeltaTime, const ABaseCharacter& Character, const UBaseCharacterMovementComponent& MoveComponent, bool bForcePrepareAll)
{
	// Add pending sources
	{
		RootMotionSources.Append(PendingAddRootMotionSources);
		PendingAddRootMotionSources.Empty();
	}

	// Sort by priority
	if (RootMotionSources.Num() > 1)
	{
		RootMotionSources.StableSort([](const TSharedPtr<FBaseRootMotionSource>& SourceL, const TSharedPtr<FBaseRootMotionSource>& SourceR)
			{
				if (SourceL.IsValid() && SourceR.IsValid())
				{
					return SourceL->Priority > SourceR->Priority;
				}
				checkf(false, TEXT("RootMotionSources being sorted are invalid pointers"));
				return true;
			});
	}

	// Prepare active sources
	{
		bHasOverrideSources = false;
		bHasOverrideSourcesWithIgnoreZAccumulate = false;
		bHasAdditiveSources = false;
		LastAccumulatedSettings.Clear();

		// Go through all sources, prepare them so that they each save off how much they're going to contribute this frame
		for (TSharedPtr<FBaseRootMotionSource>& RootMotionSource : RootMotionSources)
		{
			if (RootMotionSource.IsValid())
			{
				if (!RootMotionSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::Prepared) || bForcePrepareAll)
				{
					float SimulationTime = DeltaTime;

					// If we've received authoritative correction to root motion state, we need to
					// increase simulation time to catch up to where we were
					if (RootMotionSource->bNeedsSimulatedCatchup)
					{
						float CorrectionDelta = RootMotionSource->PreviousTime - RootMotionSource->CurrentTime;
						if (CorrectionDelta > 0.f)
						{
							// In the simulated catch-up case, we are a simulated proxy receiving authoritative state
							// from the server version of the root motion. When receiving authoritative state we could:
							// 1) Always snap precisely to authoritative time
							//     - But with latency, this could just result in unnecessary jerkiness for correction.
							//       We're completely reliant on mesh smoothing for fix-up.
							// 2) Always maintain Time as "authoritative" on simulated
							//     - But if the application of the root motion source was off, we're just maintaining
							//       that inconsistency indefinitely, and any additional error/hitches on the server's
							//       or latency's part only push the authoritative and simulated versions further apart
							//       and we have no mechanism for reconciling them over time.
							// 3) Split it down the middle - move towards authoritative state while not doing full snaps
							//    so that we use both internal "simulated catchup" smoothing along with mesh smoothing
							//    and over time we correct towards authoritative time
							// Below is #3
							const float MaxTimeDeltaCorrectionPercent = 0.5f; // Max percent of time mismatch to make up per authoritative update
							const float MaxTimeDeltaCorrectionAbsolute = 0.5f; // Amount of time in seconds we can erase on simulated
							
							CorrectionDelta = FMath::Min(CorrectionDelta * MaxTimeDeltaCorrectionPercent, MaxTimeDeltaCorrectionAbsolute);

							const float PreviousSimulationTime = SimulationTime;

							SimulationTime += CorrectionDelta;

							UE_LOG(LogRootMotion, VeryVerbose, TEXT("Adjusting SimulationTime due to bNeedsSimulatedCatchup before Preparing RootMotionSource %s from %f to %f"), 
								*RootMotionSource->ToSimpleString(), PreviousSimulationTime, SimulationTime);
						}
					}

					// Handle partial ticks
					{
						// Start of root motion (Root motion StartTime vs. character movement time)
						{
							const bool bRootMotionHasNotStarted = RootMotionSource->GetTime() == 0.f;
							const bool bRootMotionHasValidStartTime = RootMotionSource->IsStartTimeValid();
							if (bRootMotionHasNotStarted && bRootMotionHasValidStartTime)
							{
								float CharacterMovementTime = -1.f;
								if (Character.GetLocalRole() == ROLE_AutonomousProxy)
								{
									const FNetworkPredictionData_Client_Character* ClientData = MoveComponent.HasPredictionData_Client() ? static_cast<FNetworkPredictionData_Client_Character*>(MoveComponent.GetPredictionData_Client()) : nullptr;
									if (ClientData)
									{
										if (!Character.bClientUpdating) 
										{
											CharacterMovementTime = ClientData->CurrentTimeStamp;
										}
										else
										{
											// TODO: To enable this during bClientUpdating case, we need to have access
											// to the CurrentTimeStamp that happened during the original move.
											// We could add this in saved move replay PrepMoveFor() logic.
											// Right now we don't have this, but it should only affect first server move
											// of root motion corrections which shouldn't have corrections in the common case
											// (we don't have cases where we set StartTime to be in the distant future yet)
										}
									}
								}
								else if (Character.GetLocalRole() == ROLE_Authority)
								{
									const FNetworkPredictionData_Server_Character* ServerData = MoveComponent.HasPredictionData_Server() ? static_cast<FNetworkPredictionData_Server_Character*>(MoveComponent.GetPredictionData_Server()) : nullptr;
									if (ServerData)
									{
										CharacterMovementTime = ServerData->CurrentClientTimeStamp - DeltaTime; // CurrentClientTimeStamp is the client time AFTER this DeltaTime move
									}
								}

								const bool bHasValidMovementTime = CharacterMovementTime >= 0.f;
								const bool bHasSourceNotStarted = RootMotionSource->GetStartTime() > CharacterMovementTime;
								if (bHasValidMovementTime && bHasSourceNotStarted)
								{
									const float PreviousSimulationTime = SimulationTime;

									// Our StartTime hasn't yet hit, we'll need to adjust SimulationTime
									const float EndCharacterMovementTime = CharacterMovementTime + SimulationTime;
									if (EndCharacterMovementTime <= RootMotionSource->GetStartTime())
									{
										// We won't reach the StartTime this frame at all, so we don't need any SimulationTime done
										SimulationTime = 0.f;
										UE_LOG(LogRootMotion, VeryVerbose, TEXT("Adjusting SimulationTime due to StartTime not reachable this tick before Preparing RootMotionSource %s from %f to %f"), 
											*RootMotionSource->ToSimpleString(), PreviousSimulationTime, SimulationTime);
									}
									else
									{
										// Root motion will kick in partway through this tick, adjust SimulationTime
										// so that the amount of root motion applied matches what length of time it
										// should have been active (need to do this because root motions are either
										// on for an entire movement tick or not at all)
										SimulationTime = EndCharacterMovementTime - RootMotionSource->GetStartTime();
										UE_LOG(LogRootMotion, VeryVerbose, TEXT("Adjusting SimulationTime due to StartTime reachable partway through tick before Preparing RootMotionSource %s from %f to %f"), 
											*RootMotionSource->ToSimpleString(), PreviousSimulationTime, SimulationTime);
									}
								}
							}
						}

						// End of root motion
						if (RootMotionSource->IsTimeOutEnabled() && !RootMotionSource->Settings.HasFlag(EBaseRootMotionSourceSettingsFlags::DisablePartialEndTick))
						{
							const float Duration = RootMotionSource->GetDuration();
							if (RootMotionSource->GetTime() + SimulationTime >= Duration)
							{
								const float PreviousSimulationTime = SimulationTime;

								// Upcoming tick will go beyond the intended duration, if we kept
								// SimulationTime unchanged we would get more movement than was
								// intended so we clamp it to duration
								SimulationTime = Duration - RootMotionSource->GetTime() + UE_KINDA_SMALL_NUMBER; // Plus a little to make sure we push it over Duration
								UE_LOG(LogRootMotion, VeryVerbose, TEXT("Adjusting SimulationTime due to Duration reachable partway through tick before Preparing RootMotionSource %s from %f to %f"), 
									*RootMotionSource->ToSimpleString(), PreviousSimulationTime, SimulationTime);
							}
						}
					}

					// Sanity check resulting SimulationTime
					SimulationTime = FMath::Max(SimulationTime, 0.f);

					// Do the Preparation (calculates root motion transforms to be applied)
					RootMotionSource->bSimulatedNeedsSmoothing = false;
					RootMotionSource->PrepareRootMotion(SimulationTime, DeltaTime, Character, MoveComponent);
					LastAccumulatedSettings += RootMotionSource->Settings;
					RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::Prepared);

#if ROOT_MOTION_DEBUG
					if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
					{
						FString AdjustedDebugString = FString::Printf(TEXT("PrepareRootMotion Prepared RootMotionSource(%s)"),
							*RootMotionSource->ToSimpleString());
						BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
					}
#endif

					RootMotionSource->bNeedsSimulatedCatchup = false;
				}
				else // if (!RootMotionSource->Status.HasFlag(EBaseRootMotionSourceStatusFlags::Prepared) || bForcePrepareAll)
				{
#if ROOT_MOTION_DEBUG
					if (BaseRootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
					{
						FString AdjustedDebugString = FString::Printf(TEXT("PrepareRootMotion AlreadyPrepared RootMotionSource(%s)"),
							*RootMotionSource->ToSimpleString());
						BaseRootMotionSourceDebug::PrintOnScreen(Character, AdjustedDebugString);
					}
#endif
				}

				if (RootMotionSource->AccumulateMode == EBaseRootMotionAccumulateMode::Additive)
				{
					bHasAdditiveSources = true;
				}
				else if (RootMotionSource->AccumulateMode == EBaseRootMotionAccumulateMode::Override)
				{
					bHasOverrideSources = true;

					if (RootMotionSource->Settings.HasFlag(EBaseRootMotionSourceSettingsFlags::IgnoreZAccumulate))
					{
						bHasOverrideSourcesWithIgnoreZAccumulate = true;
					}
				}
			}
		}
	}
}

void FBaseRootMotionSourceGroup::AccumulateOverrideRootMotionVelocity
	(
		float DeltaTime, 
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent, 
		FVector& InOutVelocity
	) const
{
	AccumulateRootMotionVelocity(EBaseRootMotionAccumulateMode::Override, DeltaTime, Character, MoveComponent, InOutVelocity);
}

void FBaseRootMotionSourceGroup::AccumulateAdditiveRootMotionVelocity
	(
		float DeltaTime, 
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent, 
		FVector& InOutVelocity
	) const
{
	AccumulateRootMotionVelocity(EBaseRootMotionAccumulateMode::Additive, DeltaTime, Character, MoveComponent, InOutVelocity);
}

void FBaseRootMotionSourceGroup::AccumulateRootMotionVelocity
	(
		EBaseRootMotionAccumulateMode RootMotionType,
		float DeltaTime, 
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent, 
		FVector& InOutVelocity
	) const
{
	check(RootMotionType == EBaseRootMotionAccumulateMode::Additive || RootMotionType == EBaseRootMotionAccumulateMode::Override);

	// Go through all sources, accumulate their contribution to root motion
	for (const auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->AccumulateMode == RootMotionType)
		{
			AccumulateRootMotionVelocityFromSource(*RootMotionSource, DeltaTime, Character, MoveComponent, InOutVelocity);

			// For Override root motion, we apply the highest priority override and ignore the rest
			if (RootMotionSource->AccumulateMode == EBaseRootMotionAccumulateMode::Override)
			{
				break;
			}
		}
	}
}

void FBaseRootMotionSourceGroup::AccumulateRootMotionVelocityFromSource
	(
		const FBaseRootMotionSource& RootMotionSource,
		float DeltaTime, 
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent, 
		FVector& InOutVelocity
	) const
{
	FRootMotionMovementParams RootMotionParams = RootMotionSource.RootMotionParams;

	// Transform RootMotion if needed (world vs local space)
	if (RootMotionSource.bInLocalSpace && MoveComponent.UpdatedComponent)
	{
		RootMotionParams.Set( RootMotionParams.GetRootMotionTransform() * MoveComponent.UpdatedComponent->GetComponentToWorld().GetRotation() );
	}

	const FVector RootMotionVelocity = RootMotionParams.GetRootMotionTransform().GetTranslation();

	const FVector InputVelocity = InOutVelocity;
	if (RootMotionSource.AccumulateMode == EBaseRootMotionAccumulateMode::Override)
	{
		InOutVelocity = RootMotionVelocity;
	}
	else if (RootMotionSource.AccumulateMode == EBaseRootMotionAccumulateMode::Additive)
	{
		InOutVelocity += RootMotionVelocity;
	}
	if (RootMotionSource.Settings.HasFlag(EBaseRootMotionSourceSettingsFlags::IgnoreZAccumulate))
	{
		InOutVelocity.Z = InputVelocity.Z;
	}
}

bool FBaseRootMotionSourceGroup::GetOverrideRootMotionRotation
	(
		float DeltaTime, 
		const ABaseCharacter& Character, 
		const UBaseCharacterMovementComponent& MoveComponent, 
		FQuat& OutRotation
	) const
{
	for (const auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->AccumulateMode == EBaseRootMotionAccumulateMode::Override)
		{
			OutRotation = RootMotionSource->RootMotionParams.GetRootMotionTransform().GetRotation();
			return !OutRotation.IsIdentity();
		}
	}
	return false;
}

bool FBaseRootMotionSourceGroup::NeedsSimulatedSmoothing() const
{
	for (const auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->bSimulatedNeedsSmoothing)
		{
			return true;
		}
	}
	return false;
}

void FBaseRootMotionSourceGroup::SetPendingRootMotionSourceMinStartTimes(float NewStartTime)
{
	for (auto& RootMotionSource : PendingAddRootMotionSources)
	{
		if (RootMotionSource.IsValid())
		{
			const float PreviousStartTime = RootMotionSource->StartTime;
			const float MinStartTime = NewStartTime;
			RootMotionSource->StartTime = FMath::Max(PreviousStartTime, NewStartTime);
			if (PreviousStartTime != RootMotionSource->StartTime)
			{
				UE_LOG(LogRootMotion, VeryVerbose, TEXT("Pending RootMotionSource %s starting time modification: previous: %f new: %f"), *RootMotionSource->ToSimpleString(), PreviousStartTime, RootMotionSource->StartTime);
			}
		}
	}
}

void FBaseRootMotionSourceGroup::ApplyTimeStampReset(float DeltaTime)
{
	check(-DeltaTime > RootMotionSource_InvalidStartTime);

	for (auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->IsStartTimeValid())
		{
			const float PreviousStartTime = RootMotionSource->StartTime;
			RootMotionSource->StartTime -= DeltaTime;
			UE_LOG(LogRootMotion, VeryVerbose, TEXT("Applying time stamp reset to RootMotionSource %s StartTime: previous(%f), new(%f)"), *RootMotionSource->ToSimpleString(), PreviousStartTime, RootMotionSource->StartTime);
		}
	}

	for (auto& RootMotionSource : PendingAddRootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->IsStartTimeValid())
		{
			const float PreviousStartTime = RootMotionSource->StartTime;
			RootMotionSource->StartTime -= DeltaTime;
			UE_LOG(LogRootMotion, VeryVerbose, TEXT("Applying time stamp reset to PendingAddRootMotionSource %s StartTime: previous(%f), new(%f)"), *RootMotionSource->ToSimpleString(), PreviousStartTime, RootMotionSource->StartTime);
		}
	}
}

uint16 FBaseRootMotionSourceGroup::ApplyRootMotionSource(TSharedPtr<FBaseRootMotionSource> SourcePtr)
{
	if (ensure(SourcePtr.IsValid()))
	{
		// Get valid localID
		// Note: Current ID method could produce duplicate IDs "in flight" at one time
		// if you have one root motion source applied while 2^16-1 other root motion sources
		// get applied and it's still applied and it happens that the 2^16-1th root motion
		// source is applied on this character movement component. 
		// This was preferred over the complexity of ensuring unique IDs.
		static uint16 LocalIDGenerator = 0;
		uint16 LocalID = ++LocalIDGenerator;
		if (LocalID == (uint16)EBaseRootMotionSourceID::Invalid)
		{
			LocalID = ++LocalIDGenerator;
		}
		SourcePtr->LocalID = LocalID;

		// Apply to pending so that next Prepare it gets added to "active"
		PendingAddRootMotionSources.Add(SourcePtr);
		UE_LOG(LogRootMotion, VeryVerbose, TEXT("RootMotionSource added to Pending: [%u] %s"), LocalID, *SourcePtr->ToSimpleString());

		return LocalID;
	}

	return (uint16)EBaseRootMotionSourceID::Invalid;
}

TSharedPtr<FBaseRootMotionSource> FBaseRootMotionSourceGroup::GetRootMotionSource(FName InstanceName)
{
	for (const auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->InstanceName == InstanceName)
		{
			return TSharedPtr<FBaseRootMotionSource>(RootMotionSource);
		}
	}

	for (const auto& RootMotionSource : PendingAddRootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->InstanceName == InstanceName)
		{
			return TSharedPtr<FBaseRootMotionSource>(RootMotionSource);
		}
	}

	return TSharedPtr<FBaseRootMotionSource>(nullptr);
}

TSharedPtr<FBaseRootMotionSource> FBaseRootMotionSourceGroup::GetRootMotionSourceByID(uint16 RootMotionSourceID)
{
	for (const auto& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->LocalID == RootMotionSourceID)
		{
			return TSharedPtr<FBaseRootMotionSource>(RootMotionSource);
		}
	}

	for (const auto& RootMotionSource : PendingAddRootMotionSources)
	{
		if (RootMotionSource.IsValid() && RootMotionSource->LocalID == RootMotionSourceID)
		{
			return TSharedPtr<FBaseRootMotionSource>(RootMotionSource);
		}
	}

	return TSharedPtr<FBaseRootMotionSource>(nullptr);
}

void FBaseRootMotionSourceGroup::RemoveRootMotionSource(FName InstanceName)
{
	if (!InstanceName.IsNone()) // Don't allow removing None since that's the default
	{
		for (const auto& RootMotionSource : RootMotionSources)
		{
			if (RootMotionSource.IsValid() && RootMotionSource->InstanceName == InstanceName)
			{
				RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			}
		}

		for (const auto& RootMotionSource : PendingAddRootMotionSources)
		{
			if (RootMotionSource.IsValid() && RootMotionSource->InstanceName == InstanceName)
			{
				RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			}
		}
	}
}

void FBaseRootMotionSourceGroup::RemoveRootMotionSourceByID(uint16 RootMotionSourceID)
{
	if (RootMotionSourceID != (uint16)EBaseRootMotionSourceID::Invalid)
	{
		for (const auto& RootMotionSource : RootMotionSources)
		{
			if (RootMotionSource.IsValid() && RootMotionSource->LocalID == RootMotionSourceID)
			{
				RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			}
		}

		for (const auto& RootMotionSource : PendingAddRootMotionSources)
		{
			if (RootMotionSource.IsValid() && RootMotionSource->LocalID == RootMotionSourceID)
			{
				RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
			}
		}
	}
}

void FBaseRootMotionSourceGroup::UpdateStateFrom(const FBaseRootMotionSourceGroup& GroupToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	bIsAdditiveVelocityApplied = GroupToTakeStateFrom.bIsAdditiveVelocityApplied;
	LastPreAdditiveVelocity = GroupToTakeStateFrom.LastPreAdditiveVelocity;

	// If we have a PendingAdd root motion source that is already active in GroupToTakeStateFrom, make it active
	PendingAddRootMotionSources.RemoveAll([this, &GroupToTakeStateFrom](const TSharedPtr<FBaseRootMotionSource>& RootSource) 
		{ 
			if (RootSource.IsValid())
			{
				if (RootSource->LocalID != (uint16)EBaseRootMotionSourceID::Invalid)
				{
					for (const TSharedPtr<FBaseRootMotionSource>& TakeFromRootMotionSource : GroupToTakeStateFrom.RootMotionSources)
					{
						if (TakeFromRootMotionSource.IsValid() && (RootSource->LocalID == TakeFromRootMotionSource->LocalID))
						{
							// Matches, move to active and remove from pending
							UE_LOG(LogRootMotion, VeryVerbose, TEXT("UpdateStateFream moving PendingAdd RMS to active: %s"), *RootSource->ToSimpleString());
							RootMotionSources.Add(RootSource);
							return true;
						}
					}
				}
			}
			return false;
		});

	// For each matching Source in GroupToTakeStateFrom, move state over to this group's Sources
	// Can do all matching with LocalID only, since anything passed into this function should have
	// already been "matched" to LocalIDs
	for (const TSharedPtr<FBaseRootMotionSource>& TakeFromRootMotionSource : GroupToTakeStateFrom.RootMotionSources)
	{
		if (TakeFromRootMotionSource.IsValid() && (TakeFromRootMotionSource->LocalID != (uint16)EBaseRootMotionSourceID::Invalid))
		{
			for (const TSharedPtr<FBaseRootMotionSource>& RootMotionSource : RootMotionSources)
			{
				if (RootMotionSource.IsValid() && (RootMotionSource->LocalID == TakeFromRootMotionSource->LocalID))
				{
					// We rely on the 'Matches' rule to be exact, verify that it is still correct here.
					// If not, we're matching different root motion sources, or we're using properties that change over time for matching.
					if (!RootMotionSource->Matches(TakeFromRootMotionSource.Get()))
					{
						ensureMsgf(false, TEXT("UpdateStateFrom RootMotionSource(%s) has the same LocalID(%d) as a non-matching TakeFromRootMotionSource(%s)!"),
							*RootMotionSource->ToSimpleString(), RootMotionSource->LocalID, *TakeFromRootMotionSource->ToSimpleString());
						
						// See if multiple local sources match this ServerRootMotionSource by rules
						UE_LOG(LogRootMotion, Warning, TEXT("Finding Matches by rules for TakeFromRootMotionSource(%s)"), *TakeFromRootMotionSource->ToSimpleString());
						for (int32 Index=0; Index<RootMotionSources.Num(); Index++)
						{
							const TSharedPtr<FBaseRootMotionSource>& TestRootMotionSource = RootMotionSources[Index];
							if (TestRootMotionSource.IsValid())
							{
								UE_LOG(LogRootMotion, Warning, TEXT("[%d/%d] Matches(%s) ? (%d)"),
									Index + 1, RootMotionSources.Num(), *TestRootMotionSource->ToSimpleString(), TestRootMotionSource->Matches(TakeFromRootMotionSource.Get()));
							}
						}

						// See if multiple local sources match this ServerRootMotionSource by ID
						UE_LOG(LogRootMotion, Warning, TEXT("Finding Matches by ID for TakeFromRootMotionSource(%s)"), *TakeFromRootMotionSource->ToSimpleString());
						for (int32 Index = 0; Index < RootMotionSources.Num(); Index++)
						{
							const TSharedPtr<FBaseRootMotionSource>& TestRootMotionSource = RootMotionSources[Index];
							if (TestRootMotionSource.IsValid())
							{
								UE_LOG(LogRootMotion, Warning, TEXT("[%d/%d] Matches(%s) ? (%d)"),
									Index + 1, RootMotionSources.Num(), *TestRootMotionSource->ToSimpleString(), TestRootMotionSource->LocalID == TakeFromRootMotionSource->LocalID);
							}
						}

						continue;
					}

					const bool bSuccess = RootMotionSource->UpdateStateFrom(TakeFromRootMotionSource.Get(), bMarkForSimulatedCatchup);
					if (bSuccess)
					{
						// If we've updated state, we'll need prepared before being able to contribute
						RootMotionSource->Status.UnSetFlag(EBaseRootMotionSourceStatusFlags::Prepared);

						UE_LOG(LogRootMotion, VeryVerbose, TEXT("RootMotionSource UpdatedState: %s"), *RootMotionSource->ToSimpleString());
					}
					else
					{
						RootMotionSource->Status.SetFlag(EBaseRootMotionSourceStatusFlags::MarkedForRemoval);
						UE_LOG(LogRootMotion, Warning,  TEXT("RootMotionSource failed to be updated from matching Source, marking for removal"));
					}
				}
			}
		}
	}
}

struct FBaseRootMotionSourceDeleter
{
	FORCEINLINE void operator()(FBaseRootMotionSource* Object) const
	{
		check(Object);
		UScriptStruct* ScriptStruct = Object->GetScriptStruct();
		check(ScriptStruct);
		ScriptStruct->DestroyStruct(Object);
		FMemory::Free(Object);
	}
};

void FBaseRootMotionSourceGroup::NetSerializeRMSArray(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess, TArray< TSharedPtr<FBaseRootMotionSource> >& RootMotionSourceArray, uint8 MaxNumRootMotionSourcesToSerialize/* = MAX_uint8*/)
{
	uint8 SourcesNum;
	if (Ar.IsSaving())
	{
		UE_CLOG(RootMotionSourceArray.Num() > MaxNumRootMotionSourcesToSerialize, LogRootMotion, Warning, TEXT("Too many root motion sources (%d!) to net serialize. Clamping to %d"),
			RootMotionSourceArray.Num(), MaxNumRootMotionSourcesToSerialize);
		SourcesNum = FMath::Min<int32>(RootMotionSourceArray.Num(), MaxNumRootMotionSourcesToSerialize);
	}
	Ar << SourcesNum;
	if (Ar.IsLoading())
	{
		RootMotionSourceArray.SetNumZeroed(SourcesNum);
	}

	for (int32 i = 0; i < SourcesNum && !Ar.IsError(); ++i)
	{
		TCheckedObjPtr<UScriptStruct> ScriptStruct = RootMotionSourceArray[i].IsValid() ? RootMotionSourceArray[i]->GetScriptStruct() : nullptr;
		UScriptStruct* ScriptStructLocal = ScriptStruct.Get();
		Ar << ScriptStruct;

		if (ScriptStruct.IsValid())
		{
			// Restrict replication to derived classes of FBaseRootMotionSource for security reasons:
			// If FBaseRootMotionSourceGroup is replicated through a Server RPC, we need to prevent clients from sending us
			// arbitrary ScriptStructs due to the allocation/reliance on GetCppStructOps below which could trigger a server crash
			// for invalid structs. All provided sources are direct children of RMS and we never expect to have deep hierarchies
			// so this should not be too costly
			bool bIsDerivedFromBaseRMS = false;
			UStruct* CurrentSuperStruct = ScriptStruct->GetSuperStruct();
			while (CurrentSuperStruct)
			{
				if (CurrentSuperStruct == FBaseRootMotionSource::StaticStruct())
				{
					bIsDerivedFromBaseRMS = true;
					break;
				}
				CurrentSuperStruct = CurrentSuperStruct->GetSuperStruct();
			}

			if (bIsDerivedFromBaseRMS)
			{
				if (Ar.IsLoading())
				{
					if (RootMotionSourceArray[i].IsValid() && ScriptStructLocal == ScriptStruct.Get())
					{
						// What we have locally is the same type as we're being serialized into, so we don't need to
						// reallocate - just use existing structure
					}
					else
					{
						// For now, just reset/reallocate the data when loading.
						// Longer term if we want to generalize this and use it for property replication, we should support
						// only reallocating when necessary
						FBaseRootMotionSource* NewSource = (FBaseRootMotionSource*)FMemory::Malloc(ScriptStruct->GetCppStructOps()->GetSize());
						ScriptStruct->InitializeStruct(NewSource);

						RootMotionSourceArray[i] = TSharedPtr<FBaseRootMotionSource>(NewSource, FBaseRootMotionSourceDeleter());
					}
				}

				void* ContainerPtr = RootMotionSourceArray[i].Get();

				if (ScriptStruct->StructFlags & STRUCT_NetSerializeNative)
				{
					ScriptStruct->GetCppStructOps()->NetSerialize(Ar, Map, bOutSuccess, RootMotionSourceArray[i].Get());
				}
				else
				{
					checkf(false, TEXT("Serializing RootMotionSource without NetSerializeNative - not supported!"));
				}
			}
			else
			{
				UE_LOG(LogRootMotion, Error, TEXT("FBaseRootMotionSourceGroup::NetSerialize: ScriptStruct not derived from FBaseRootMotionSource attempted to serialize."));
				Ar.SetError();
				break;
			}
		}
		else if (ScriptStruct.IsError())
		{
			UE_LOG(LogRootMotion, Error, TEXT("FBaseRootMotionSourceGroup::NetSerialize: Invalid ScriptStruct serialized."));
			Ar.SetError();
			break;
		}
	}

}

bool FBaseRootMotionSourceGroup::NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess, uint8 MaxNumRootMotionSourcesToSerialize/* = MAX_uint8*/)
{
	FArchive_Serialize_BitfieldBool(Ar, bHasAdditiveSources);
	FArchive_Serialize_BitfieldBool(Ar, bHasOverrideSources);
	FArchive_Serialize_BitfieldBool(Ar, bHasOverrideSourcesWithIgnoreZAccumulate);
	LastPreAdditiveVelocity.NetSerialize(Ar, Map, bOutSuccess);
	FArchive_Serialize_BitfieldBool(Ar, bIsAdditiveVelocityApplied);
	Ar << LastAccumulatedSettings.Flags;

	uint8 NumRootMotionSourcesToSerialize = FMath::Min<int32>(RootMotionSources.Num(), MaxNumRootMotionSourcesToSerialize);
	uint8 NumPendingAddRootMotionSourcesToSerialize = NumRootMotionSourcesToSerialize < MaxNumRootMotionSourcesToSerialize ? MaxNumRootMotionSourcesToSerialize - NumRootMotionSourcesToSerialize : 0;
	NetSerializeRMSArray(Ar, Map, bOutSuccess, RootMotionSources, NumRootMotionSourcesToSerialize);
	NetSerializeRMSArray(Ar, Map, bOutSuccess, PendingAddRootMotionSources, NumPendingAddRootMotionSourcesToSerialize);

	if (Ar.IsError())
	{
		// Something bad happened, make sure to not return invalid shared ptrs
		for (int32 i = RootMotionSources.Num() - 1; i >= 0; --i)
		{
			if (RootMotionSources[i].IsValid() == false)
			{
				RootMotionSources.RemoveAt(i);
			}
		}
		for (int32 i = PendingAddRootMotionSources.Num() - 1; i >= 0; --i)
		{
			if (PendingAddRootMotionSources[i].IsValid() == false)
			{
				PendingAddRootMotionSources.RemoveAt(i);
			}
		}
		bOutSuccess = false;
		return false;
	}

	bOutSuccess = true;
	return true;
}

void FBaseRootMotionSourceGroup::CullInvalidSources()
{
	RootMotionSources.RemoveAll([](const TSharedPtr<FBaseRootMotionSource>& RootSource) 
		{ 
			if (RootSource.IsValid())
			{
				if (RootSource->LocalID != (uint16)EBaseRootMotionSourceID::Invalid)
				{
					return false;
				}
				UE_LOG(LogRootMotion, VeryVerbose, TEXT("RootMotionSource being culled as invalid: %s"), *RootSource->ToSimpleString());
			}
			return true;
		});
}

void FBaseRootMotionSourceGroup::Clear()
{
	RootMotionSources.Empty();
	PendingAddRootMotionSources.Empty();
	bIsAdditiveVelocityApplied = false;
	bHasAdditiveSources = false;
	bHasOverrideSources = false;
	bHasOverrideSourcesWithIgnoreZAccumulate = false;
	LastAccumulatedSettings.Clear();
}

FBaseRootMotionSourceGroup& FBaseRootMotionSourceGroup::operator=(const FBaseRootMotionSourceGroup& Other)
{
	// Perform deep copy of this Group
	if (this != &Other)
	{
		// Deep copy Sources
		RootMotionSources.Empty(Other.RootMotionSources.Num());
		for (int i = 0; i < Other.RootMotionSources.Num(); ++i)
		{
			if (Other.RootMotionSources[i].IsValid())
			{
				FBaseRootMotionSource* CopyOfSourcePtr = Other.RootMotionSources[i]->Clone();
				RootMotionSources.Add(TSharedPtr<FBaseRootMotionSource>(CopyOfSourcePtr));
			}
			else
			{
				UE_LOG(LogRootMotion, Warning, TEXT("RootMotionSourceGroup::operator= trying to copy bad Other RMS"));
			}
		}

		// Deep copy PendingAdd sources
		PendingAddRootMotionSources.Empty(Other.PendingAddRootMotionSources.Num());
		for (int i = 0; i < Other.PendingAddRootMotionSources.Num(); ++i)
		{
			if (Other.PendingAddRootMotionSources[i].IsValid())
			{
				FBaseRootMotionSource* CopyOfSourcePtr = Other.PendingAddRootMotionSources[i]->Clone();
				PendingAddRootMotionSources.Add(TSharedPtr<FBaseRootMotionSource>(CopyOfSourcePtr));
			}
			else
			{
				UE_LOG(LogRootMotion, Warning, TEXT("RootMotionSourceGroup::operator= trying to copy bad Other PendingAdd"));
			}
		}

		bHasAdditiveSources = Other.bHasAdditiveSources;
		bHasOverrideSources = Other.bHasOverrideSources;
		bHasOverrideSourcesWithIgnoreZAccumulate = Other.bHasOverrideSourcesWithIgnoreZAccumulate;
		LastPreAdditiveVelocity = Other.LastPreAdditiveVelocity;
		bIsAdditiveVelocityApplied = Other.bIsAdditiveVelocityApplied;
		LastAccumulatedSettings = Other.LastAccumulatedSettings;
	}
	return *this;
}

bool FBaseRootMotionSourceGroup::operator==(const FBaseRootMotionSourceGroup& Other) const
{
	if (bHasAdditiveSources != Other.bHasAdditiveSources || 
		bHasOverrideSources != Other.bHasOverrideSources ||
		bHasOverrideSourcesWithIgnoreZAccumulate != Other.bHasOverrideSourcesWithIgnoreZAccumulate ||
		bIsAdditiveVelocityApplied != Other.bIsAdditiveVelocityApplied ||
		!LastPreAdditiveVelocity.Equals(Other.LastPreAdditiveVelocity, 1.f))
	{
		return false;
	}

	// Both invalid structs or both valid and Pointer compare (???) // deep comparison equality
	if (RootMotionSources.Num() != Other.RootMotionSources.Num())
	{
		return false;
	}
	if (PendingAddRootMotionSources.Num() != Other.PendingAddRootMotionSources.Num())
	{
		return false;
	}
	for (int32 i = 0; i < RootMotionSources.Num(); ++i)
	{
		if (RootMotionSources[i].IsValid() == Other.RootMotionSources[i].IsValid())
		{
			if (RootMotionSources[i].IsValid())
			{
				if (!RootMotionSources[i]->MatchesAndHasSameState(Other.RootMotionSources[i].Get()))
				{
					return false; // They're valid and don't match/have same state
				}
			}
		}
		else
		{
			return false; // Mismatch in validity
		}
	}
	for (int32 i = 0; i < PendingAddRootMotionSources.Num(); ++i)
	{
		if (PendingAddRootMotionSources[i].IsValid() == Other.PendingAddRootMotionSources[i].IsValid())
		{
			if (PendingAddRootMotionSources[i].IsValid())
			{
				if (!PendingAddRootMotionSources[i]->MatchesAndHasSameState(Other.PendingAddRootMotionSources[i].Get()))
				{
					return false; // They're valid and don't match/have same state
				}
			}
		}
		else
		{
			return false; // Mismatch in validity
		}
	}
	return true;
}

bool FBaseRootMotionSourceGroup::operator!=(const FBaseRootMotionSourceGroup& Other) const
{
	return !(FBaseRootMotionSourceGroup::operator==(Other));
}

void FBaseRootMotionSourceGroup::AddStructReferencedObjects(FReferenceCollector& Collector) const
{
	for (const TSharedPtr<FBaseRootMotionSource>& RootMotionSource : RootMotionSources)
	{
		if (RootMotionSource.IsValid())
		{
			RootMotionSource->AddReferencedObjects(Collector);
		}
	}

	for (const TSharedPtr<FBaseRootMotionSource>& RootMotionSource : PendingAddRootMotionSources)
	{
		if (RootMotionSource.IsValid())
		{
			RootMotionSource->AddReferencedObjects(Collector);
		}
	}
}

