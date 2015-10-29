// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "MeshPaintPrivatePCH.h"
#include "MeshPaintSplineMeshAdapter.h"

#include "StaticMeshResources.h"
#include "MeshPaintEdMode.h"
#include "Components/SplineMeshComponent.h"

//////////////////////////////////////////////////////////////////////////
// FMeshPaintGeometryAdapterForSplineMeshes

bool FMeshPaintGeometryAdapterForSplineMeshes::Construct(UMeshComponent* InComponent, int32 InPaintingMeshLODIndex, int32 InUVChannelIndex)
{
	bool bResult = FMeshPaintGeometryAdapterForStaticMeshes::Construct(InComponent, InPaintingMeshLODIndex, InUVChannelIndex);

	if (bResult)
	{
		// Cache deformed spline mesh vertices for quick lookup during painting / previewing
		USplineMeshComponent* SplineMeshComponent = Cast<USplineMeshComponent>(StaticMeshComponent);
		check(SplineMeshComponent);

		const int32 NumVertices = LODModel->PositionVertexBuffer.GetNumVertices();
		MeshVertices.AddDefaulted(NumVertices);
		for (int32 Index = 0; Index < NumVertices; Index++)
		{
			FVector Position = LODModel->PositionVertexBuffer.VertexPosition(Index);
			const FTransform SliceTransform = SplineMeshComponent->CalcSliceTransform(USplineMeshComponent::GetAxisValue(Position, SplineMeshComponent->ForwardAxis));
			USplineMeshComponent::GetAxisValue(Position, SplineMeshComponent->ForwardAxis) = 0;
			MeshVertices[Index] = SliceTransform.TransformPosition(Position);
		}
	}

	return bResult;
}

FVector FMeshPaintGeometryAdapterForSplineMeshes::GetMeshVertex(int32 Index) const
{
	return MeshVertices[Index];
}



//////////////////////////////////////////////////////////////////////////
// FMeshPaintGeometryAdapterForSplineMeshesFactory

TSharedPtr<IMeshPaintGeometryAdapter> FMeshPaintGeometryAdapterForSplineMeshesFactory::Construct(class UMeshComponent* InComponent, int32 InPaintingMeshLODIndex, int32 InUVChannelIndex) const
{
	if (USplineMeshComponent* SplineMeshComponent = Cast<USplineMeshComponent>(InComponent))
	{
		if (SplineMeshComponent->StaticMesh != nullptr)
		{
			TSharedRef<FMeshPaintGeometryAdapterForSplineMeshes> Result = MakeShareable(new FMeshPaintGeometryAdapterForSplineMeshes());
			if (Result->Construct(InComponent, InPaintingMeshLODIndex, InUVChannelIndex))
			{
				return Result;
			}
		}
	}

	return nullptr;
}
