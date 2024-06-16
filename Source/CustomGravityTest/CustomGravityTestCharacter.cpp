// Copyright Epic Games, Inc. All Rights Reserved.

#include "CustomGravityTestCharacter.h"

#include "CustomGravityTestPlayerController.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Character/BaseCharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GravityBoxAreaVolume.h"
#include "InputActionValue.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

//////////////////////////////////////////////////////////////////////////
// ACustomGravityTestCharacter

ACustomGravityTestCharacter::ACustomGravityTestCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true; // Character moves in the direction of input...	
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f); // ...at this rotation rate

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 700.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f; // The camera follows at this distance behind the character	
	CameraBoom->bUsePawnControlRotation = true; // Rotate the arm based on the controller

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName); // Attach the camera to the end of the boom and let the boom adjust to match the controller orientation
	FollowCamera->bUsePawnControlRotation = false; // Camera does not rotate relative to arm

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void ACustomGravityTestCharacter::BeginPlay()
{
	// Call the base class  
	Super::BeginPlay();

	//Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}
}

void ACustomGravityTestCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	
	UBaseCharacterMovementComponent* MovementComponent = GetCharacterMovement();
	check(MovementComponent);
	
	// Check to update possible gravity mode
	if (GetLocalRole() >= ROLE_AutonomousProxy && MovementComponent->MovementMode != EMovementMode::MOVE_None)
	{
		// Update gravity
		FOverlapResult GravityOverlapResult = FindGravityOverlap();
		if (GravityOverlapResult.Component != nullptr)
		{
			// const FVector UpVector = GravityOverlapResult.GetActor()->GetActorUpVector();
			const FVector UpVector = GravityOverlapResult.GetComponent()->GetUpVector();
			MovementComponent->SetGravityDirection(UpVector * -1.0f);
	
			if (MovementComponent->MovementMode == EMovementMode::MOVE_Flying)
				MovementComponent->SetMovementMode(EMovementMode::MOVE_Walking);

			// AActor* Base = GravityOverlapResult.GetActor();
			// UPrimitiveComponent* BasePrimitiveRoot = Cast<UPrimitiveComponent>(Base->GetRootComponent());
			// if (BasePrimitiveRoot)
			{
				SetBase(GravityOverlapResult.GetComponent());
			}
		}
		else
		{
			MovementComponent->SetMovementMode(EMovementMode::MOVE_Flying);

			SetBase(nullptr);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Input

void ACustomGravityTestCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ABaseCharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ABaseCharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ACustomGravityTestCharacter::Move);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ACustomGravityTestCharacter::Look);
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void ACustomGravityTestCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = Controller->GetControlRotation();

		// FRotator GravityRotation = ACustomGravityTestPlayerController::GetGravityRelativeRotation(Rotation, GetCharacterMovement()->GetGravityDirection());
		// FRotator RotatorForSideMovement = ACustomGravityTestPlayerController::GetGravityWorldRotation(FRotator(0.0f, GravityRotation.Yaw, GravityRotation.Roll), GetCharacterMovement()->GetGravityDirection());
		// FRotator RotatorForForwardMovement = ACustomGravityTestPlayerController::GetGravityWorldRotation(FRotator(0.0f, GravityRotation.Yaw, 0.0f), GetCharacterMovement()->GetGravityDirection());
		// const FVector ForwardDirection = FRotationMatrix(RotatorForForwardMovement).GetUnitAxis(EAxis::Y);
		// const FVector RightDirection = FRotationMatrix(RotatorForSideMovement).GetUnitAxis(EAxis::X);

		const FVector ForwardDirection = FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y);
		const FVector RightDirection = FRotationMatrix(Rotation).GetUnitAxis(EAxis::X);
		
		// add movement 
		AddMovementInput(ForwardDirection, MovementVector.X);
		AddMovementInput(RightDirection, MovementVector.Y);
	}
}

void ACustomGravityTestCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

FOverlapResult ACustomGravityTestCharacter::FindGravityOverlap() const
{
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(GravityAreaCollisionChannel);

	FCollisionQueryParams QueryParams;

	const FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());

	// Box gravity fields take priority
	TArray<FOverlapResult> Results;
	if (GetWorld()->OverlapMultiByObjectType(
		Results,
		Capsule->GetComponentLocation(),
		Capsule->GetComponentRotation().Quaternion(),
		ObjectQueryParams,
		CapsuleShape,
		QueryParams
	))
	{
		int32 BestOverlapIdx = -1;
			
		for (int32 OverlapIdx = 0; OverlapIdx < Results.Num(); ++OverlapIdx)
		{
			FOverlapResult const& O = Results[OverlapIdx];

			if (!O.Component.IsValid()) 
				continue;

			if (O.Component.Get()->IsA(UGravityBoxAreaVolume::StaticClass()))
			{
				if (BestOverlapIdx != -1)
				{
					const FOverlapResult& BestOverlap = Results[BestOverlapIdx];

					UGravityBoxAreaVolume* BestBoxArea = Cast<UGravityBoxAreaVolume>(BestOverlap.Component.Get());
					UGravityBoxAreaVolume* CurrBoxArea = Cast<UGravityBoxAreaVolume>(O.Component.Get());
					
					const FVector BestExtent = BestBoxArea->GetScaledBoxExtent();
					const FVector CurrExtent = CurrBoxArea->GetScaledBoxExtent(); 

					if (CurrExtent.SquaredLength() < BestExtent.SquaredLength())
					{
						BestOverlapIdx = OverlapIdx;
					}
				}
				else
				{
					BestOverlapIdx = OverlapIdx;
				}
			}
		}

		if (BestOverlapIdx != -1)
			return Results[BestOverlapIdx];
	}

	// Test against planet gravity fields

	// Damn
	FOverlapResult Result;
	return Result;	
}