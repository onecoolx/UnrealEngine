// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "MetalRHIPrivate.h"
#include "MetalStateCache.h"

static MTLTriangleFillMode TranslateFillMode(ERasterizerFillMode FillMode)
{
	switch (FillMode)
	{
		case FM_Wireframe:	return MTLTriangleFillModeLines;
		case FM_Point:		return MTLTriangleFillModeFill;
		default:			return MTLTriangleFillModeFill;
	};
}

static MTLCullMode TranslateCullMode(ERasterizerCullMode CullMode)
{
	switch (CullMode)
	{
		case CM_CCW:	return MTLCullModeFront;
		case CM_CW:		return MTLCullModeBack;
		default:		return MTLCullModeNone;
	}
}

FMetalStateCache::FMetalStateCache(FMetalCommandEncoder& InCommandEncoder)
: CommandEncoder(InCommandEncoder)
, BlendState(nullptr)
, DepthStencilState(nullptr)
, RasterizerState(nullptr)
, BoundShaderState(nullptr)
, StencilRef(0)
, BlendFactor(FLinearColor::Transparent)
, FrameBufferSize(CGSizeMake(0.0, 0.0))
, CurrentDrawable(nil)
#if PLATFORM_MAC
, CurrentLayer(nil)
#endif
, bHasValidRenderTarget(false)
{
	Viewport.originX = Viewport.originY = Viewport.width = Viewport.height = Viewport.znear = Viewport.zfar = 0.0;
	
	FMemory::Memzero(VertexBuffers, sizeof(VertexBuffers));
	FMemory::Memzero(VertexStrides, sizeof(VertexStrides));
	
	FMemory::Memzero(RenderTargetsInfo);
	
	//@todo-rco: What Size???
	// make a buffer for each shader type
	ShaderParameters[CrossCompiler::SHADER_STAGE_VERTEX].InitializeResources(1024*1024);
	ShaderParameters[CrossCompiler::SHADER_STAGE_PIXEL].InitializeResources(1024*1024);
	ShaderParameters[CrossCompiler::SHADER_STAGE_COMPUTE].InitializeResources(1024*1024);
}

FMetalStateCache::~FMetalStateCache()
{
	
}

void FMetalStateCache::SetBlendFactor(FLinearColor const& InBlendFactor)
{
	//if(BlendFactor != InBlendFactor) // @todo zebra
	{
		BlendFactor = InBlendFactor;
		CommandEncoder.SetBlendColor(InBlendFactor.R, InBlendFactor.G, InBlendFactor.B, InBlendFactor.A);
	}
}

void FMetalStateCache::SetStencilRef(uint32 const InStencilRef)
{
	//if(StencilRef != InStencilRef) // @todo zebra
	{
		StencilRef = InStencilRef;
		CommandEncoder.SetStencilReferenceValue(InStencilRef);
	}
}

void FMetalStateCache::SetBlendState(FMetalBlendState* InBlendState)
{
	//if(BlendState != InBlendState) // @todo zebra
	{
		BlendState = InBlendState;
		if(InBlendState)
		{
			for(uint32 RenderTargetIndex = 0;RenderTargetIndex < MaxMetalRenderTargets; ++RenderTargetIndex)
			{
				MTLRenderPipelineColorAttachmentDescriptor* Blend = BlendState->RenderTargetStates[RenderTargetIndex].BlendState;
				MTLRenderPipelineColorAttachmentDescriptor* Dest = [PipelineDesc.PipelineDescriptor.colorAttachments objectAtIndexedSubscript:RenderTargetIndex];

				if(Blend && Dest)
				{
					// assign each property manually, would be nice if this was faster
					Dest.blendingEnabled = Blend.blendingEnabled;
					Dest.sourceRGBBlendFactor = Blend.sourceRGBBlendFactor;
					Dest.destinationRGBBlendFactor = Blend.destinationRGBBlendFactor;
					Dest.rgbBlendOperation = Blend.rgbBlendOperation;
					Dest.sourceAlphaBlendFactor = Blend.sourceAlphaBlendFactor;
					Dest.destinationAlphaBlendFactor = Blend.destinationAlphaBlendFactor;
					Dest.alphaBlendOperation = Blend.alphaBlendOperation;
					Dest.writeMask = Blend.writeMask;
				}
		
				// set the hash bits for this RT
				PipelineDesc.SetHashValue(FMetalRenderPipelineDesc::BlendBitOffsets[RenderTargetIndex], NumBits_BlendState, BlendState->RenderTargetStates[RenderTargetIndex].BlendStateKey);
			}
		}
	}
}

void FMetalStateCache::SetDepthStencilState(FMetalDepthStencilState* InDepthStencilState)
{
	//if(DepthStencilState != InDepthStencilState) // @todo zebra
	{
		DepthStencilState = InDepthStencilState;
		if(InDepthStencilState)
		{
			//activate the state
			CommandEncoder.SetDepthStencilState(InDepthStencilState->State);
		}
	}
}

void FMetalStateCache::SetRasterizerState(FMetalRasterizerState* InRasterizerState)
{
	//if(RasterizerState != InRasterizerState) // @todo zebra
	{
		RasterizerState = InRasterizerState;
		if(InRasterizerState)
		{
			CommandEncoder.SetFrontFacingWinding(MTLWindingCounterClockwise);
			
			CommandEncoder.SetCullMode(TranslateCullMode(RasterizerState->State.CullMode));
			
			// no clamping
			CommandEncoder.SetDepthBias(RasterizerState->State.DepthBias, RasterizerState->State.SlopeScaleDepthBias, FLT_MAX);
			
			CommandEncoder.SetTriangleFillMode(TranslateFillMode(RasterizerState->State.FillMode));
		}
	}
}

void FMetalStateCache::SetBoundShaderState(FMetalBoundShaderState* InBoundShaderState)
{
	//if(BoundShaderState != InBoundShaderState) // @todo zebra
	{
		BoundShaderState = InBoundShaderState;
	}
}

void FMetalStateCache::SetComputeShader(FMetalComputeShader* InComputeShader)
{
	//if(ComputeShader != InComputeShader) // @todo zebra
	{
		ComputeShader = InComputeShader;
		
		// set this compute shader pipeline as the current (this resets all state, so we need to set all resources after calling this)
		CommandEncoder.SetComputePipelineState(ComputeShader->Kernel);
	}
}

void FMetalStateCache::SetRenderTargetsInfo(FRHISetRenderTargetsInfo const& InRenderTargets, id<MTLBuffer> const QueryBuffer)
{
	ConditionalSwitchToRender();

	// see if our new Info matches our previous Info
	if (NeedsToSetRenderTarget(InRenderTargets))
	{
		// back this up for next frame
		RenderTargetsInfo = InRenderTargets;
		
		// at this point, we need to fully set up an encoder/command buffer, so make a new one (autoreleased)
		MTLRenderPassDescriptor* RenderPass = [MTLRenderPassDescriptor renderPassDescriptor];
	
		// if we need to do queries, write to the supplied query buffer
		RenderPass.visibilityResultBuffer = QueryBuffer;
	
		// default to non-msaa
	    int32 OldCount = PipelineDesc.SampleCount;
		PipelineDesc.SampleCount = 0;
	
		bHasValidRenderTarget = false;
		
		uint8 ArrayTargets = 0;
		uint8 BoundTargets = 0;
		uint32 ArrayRenderLayers = UINT_MAX;
		
		bool bFramebufferSizeSet = false;
		FrameBufferSize = CGSizeMake(0.f, 0.f);
		
		for (uint32 RenderTargetIndex = 0; RenderTargetIndex < MaxMetalRenderTargets; RenderTargetIndex++)
		{
			// default to invalid
			uint8 FormatKey = 0;
			// only try to set it if it was one that was set (ie less than RenderTargetsInfo.NumColorRenderTargets)
			if (RenderTargetIndex < RenderTargetsInfo.NumColorRenderTargets && RenderTargetsInfo.ColorRenderTarget[RenderTargetIndex].Texture != nullptr)
			{
				const FRHIRenderTargetView& RenderTargetView = RenderTargetsInfo.ColorRenderTarget[RenderTargetIndex];
				FMetalSurface& Surface = *GetMetalSurfaceFromRHITexture(RenderTargetView.Texture);
				FormatKey = Surface.FormatKey;
				
				uint32 Width = FMath::Max((uint32)(Surface.SizeX >> RenderTargetView.MipIndex), (uint32)1);
				uint32 Height = FMath::Max((uint32)(Surface.SizeY >> RenderTargetView.MipIndex), (uint32)1);
				if(!bFramebufferSizeSet)
				{
					bFramebufferSizeSet = true;
					FrameBufferSize.width = Width;
					FrameBufferSize.height = Height;
				}
				else
				{
					FrameBufferSize.width = FMath::Min(FrameBufferSize.width, (CGFloat)Width);
					FrameBufferSize.height = FMath::Min(FrameBufferSize.height, (CGFloat)Height);
				}
	
				// if this is the back buffer, make sure we have a usable drawable
				ConditionalUpdateBackBuffer(Surface);
	
				BoundTargets |= 1 << RenderTargetIndex;
				
	            if (Surface.Texture == nil)
	            {
	                PipelineDesc.SampleCount = OldCount;
	                return;
	            }
	
				// user code generally passes -1 as a default, but we need 0
				uint32 ArraySliceIndex = RenderTargetView.ArraySliceIndex == 0xFFFFFFFF ? 0 : RenderTargetView.ArraySliceIndex;
				if (Surface.bIsCubemap)
				{
					ArraySliceIndex = GetMetalCubeFace((ECubeFace)ArraySliceIndex);
				}
				
				switch(Surface.Type)
				{
					case RRT_Texture2DArray:
					case RRT_Texture3D:
					case RRT_TextureCube:
						if(RenderTargetView.ArraySliceIndex == 0xFFFFFFFF)
						{
							ArrayTargets |= (1 << RenderTargetIndex);
							ArrayRenderLayers = FMath::Min(ArrayRenderLayers, Surface.GetNumFaces());
						}
						else
						{
							ArrayRenderLayers = 1;
						}
						break;
					default:
						ArrayRenderLayers = 1;
						break;
				}
	
				MTLRenderPassColorAttachmentDescriptor* ColorAttachment = [MTLRenderPassColorAttachmentDescriptor new];
	
				if (Surface.MSAATexture != nil)
				{
					// set up an MSAA attachment
					ColorAttachment.texture = Surface.MSAATexture;
					ColorAttachment.storeAction = MTLStoreActionMultisampleResolve;
					ColorAttachment.resolveTexture = Surface.Texture;
					PipelineDesc.SampleCount = Surface.MSAATexture.sampleCount;
	
					// only allow one MRT with msaa
					checkf(RenderTargetsInfo.NumColorRenderTargets == 1, TEXT("Only expected one MRT when using MSAA"));
				}
				else
				{
					// set up non-MSAA attachment
					ColorAttachment.texture = Surface.Texture;
					ColorAttachment.storeAction = GetMetalRTStoreAction(RenderTargetView.StoreAction);
					PipelineDesc.SampleCount = 1;
				}
				
				ColorAttachment.level = RenderTargetView.MipIndex;
				if(Surface.Type == RRT_Texture3D)
				{
					ColorAttachment.depthPlane = ArraySliceIndex;
				}
				else
				{
					ColorAttachment.slice = ArraySliceIndex;
				}
				
				ColorAttachment.loadAction = GetMetalRTLoadAction(RenderTargetView.LoadAction);
				const FClearValueBinding& ClearValue = RenderTargetsInfo.ColorRenderTarget[RenderTargetIndex].Texture->GetClearBinding();
				if (ClearValue.ColorBinding == EClearBinding::EColorBound)
				{
					const FLinearColor& ClearColor = ClearValue.GetClearColor();
					ColorAttachment.clearColor = MTLClearColorMake(ClearColor.R, ClearColor.G, ClearColor.B, ClearColor.A);
				}

				// assign the attachment to the slot
				[RenderPass.colorAttachments setObject:ColorAttachment atIndexedSubscript:RenderTargetIndex];
				[PipelineDesc.PipelineDescriptor.colorAttachments objectAtIndexedSubscript:RenderTargetIndex].pixelFormat = ColorAttachment.texture.pixelFormat;
	
				[ColorAttachment release];
	
				bHasValidRenderTarget = true;
			}
			else
			{
				[PipelineDesc.PipelineDescriptor.colorAttachments objectAtIndexedSubscript:RenderTargetIndex].pixelFormat = MTLPixelFormatInvalid;
			}
	
			// update the hash no matter what case (null, unused, used)
			PipelineDesc.SetHashValue(FMetalRenderPipelineDesc::RTBitOffsets[RenderTargetIndex], NumBits_RenderTargetFormat, FormatKey);
		}
		
#if METAL_API_1_1 && PLATFORM_MAC
		if(ArrayTargets)
		{
			if (ArrayTargets == BoundTargets)
			{
				RenderPass.renderTargetArrayLength = ArrayRenderLayers;
			}
			else
			{
				UE_LOG(LogMetal, Fatal, TEXT("All color render targets must be layered when performing mulit-layered rendering under Metal."));
			}
		}
#endif
	
		// default to invalid
		PipelineDesc.PipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
		PipelineDesc.PipelineDescriptor.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
		
		uint8 DepthFormatKey = 0;
		uint8 StencilFormatKey = 0;
	
		// setup depth and/or stencil
		if (RenderTargetsInfo.DepthStencilRenderTarget.Texture != nullptr)
		{
			FMetalSurface& Surface = *GetMetalSurfaceFromRHITexture(RenderTargetsInfo.DepthStencilRenderTarget.Texture);
			
			if(!bFramebufferSizeSet)
			{
				bFramebufferSizeSet = true;
				FrameBufferSize.width = Surface.SizeX;
				FrameBufferSize.height = Surface.SizeY;
			}
			else
			{
				FrameBufferSize.width = FMath::Min(FrameBufferSize.width, (CGFloat)Surface.SizeX);
				FrameBufferSize.height = FMath::Min(FrameBufferSize.height, (CGFloat)Surface.SizeY);
			}
			
			MTLPixelFormat DepthStencilFormat = Surface.Texture ? Surface.Texture.pixelFormat : MTLPixelFormatInvalid;
			
			bool bHasDepth = false;
			bool bHasStencil = false;
			switch(DepthStencilFormat)
			{
				case MTLPixelFormatDepth32Float:
					bHasDepth = true;
					break;
				case MTLPixelFormatStencil8:
					bHasStencil = true;
					break;
#if METAL_API_1_1
				case MTLPixelFormatDepth32Float_Stencil8:
					bHasDepth = true;
					bHasStencil = true;
					break;
#if PLATFORM_MAC
				case MTLPixelFormatDepth24Unorm_Stencil8:
					bHasDepth = true;
					bHasStencil = true;
					break;
#endif
#endif
				default:
					break;
			}

			float DepthClearValue = 0.0f;
			uint32 StencilClearValue = 0;
			const FClearValueBinding& ClearValue = RenderTargetsInfo.DepthStencilRenderTarget.Texture->GetClearBinding();
			if (ClearValue.ColorBinding == EClearBinding::EDepthStencilBound)
			{
				ClearValue.GetDepthStencil(DepthClearValue, StencilClearValue);
			}

			if (bHasDepth)
			{
				MTLRenderPassDepthAttachmentDescriptor* DepthAttachment = [[MTLRenderPassDepthAttachmentDescriptor alloc] init];
				
				DepthFormatKey = Surface.FormatKey;
	
				// set up the depth attachment
				DepthAttachment.texture = Surface.Texture;
				DepthAttachment.loadAction = GetMetalRTLoadAction(RenderTargetsInfo.DepthStencilRenderTarget.DepthLoadAction);
#if PLATFORM_MAC
				if(RenderTargetsInfo.DepthStencilRenderTarget.GetDepthStencilAccess().IsDepthWrite())
				{
					DepthAttachment.storeAction = GetMetalRTStoreAction(RenderTargetsInfo.DepthStencilRenderTarget.DepthStoreAction);
				}
				else
				{
					DepthAttachment.storeAction = MTLStoreActionDontCare;
				}
#else
				DepthAttachment.storeAction = GetMetalRTStoreAction(RenderTargetsInfo.DepthStencilRenderTarget.DepthStoreAction);
#endif
				DepthAttachment.clearDepth = DepthClearValue;

				PipelineDesc.PipelineDescriptor.depthAttachmentPixelFormat = DepthAttachment.texture.pixelFormat;
				if (PipelineDesc.SampleCount == 0)
				{
					PipelineDesc.SampleCount = DepthAttachment.texture.sampleCount;
				}
	
				// and assign it
				RenderPass.depthAttachment = DepthAttachment;
				[DepthAttachment release];
			}
	
			if (bHasStencil)
			{
				MTLRenderPassStencilAttachmentDescriptor* StencilAttachment = [[MTLRenderPassStencilAttachmentDescriptor alloc] init];
				
				StencilFormatKey = Surface.FormatKey;
	
				// set up the stencil attachment
				StencilAttachment.texture = Surface.StencilTexture;
				StencilAttachment.loadAction = GetMetalRTLoadAction(RenderTargetsInfo.DepthStencilRenderTarget.StencilLoadAction);
#if PLATFORM_MAC
				if(RenderTargetsInfo.DepthStencilRenderTarget.GetDepthStencilAccess().IsStencilWrite())
				{
					StencilAttachment.storeAction = GetMetalRTStoreAction(RenderTargetsInfo.DepthStencilRenderTarget.GetStencilStoreAction());
				}
				else
				{
					StencilAttachment.storeAction = MTLStoreActionDontCare;
				}
#else
				StencilAttachment.storeAction = GetMetalRTStoreAction(RenderTargetsInfo.DepthStencilRenderTarget.GetStencilStoreAction());
#endif
				StencilAttachment.clearStencil = StencilClearValue;

				PipelineDesc.PipelineDescriptor.stencilAttachmentPixelFormat = StencilAttachment.texture.pixelFormat;
				if (PipelineDesc.SampleCount == 0)
				{
					PipelineDesc.SampleCount = StencilAttachment.texture.sampleCount;
				}
	
				// and assign it
				RenderPass.stencilAttachment = StencilAttachment;
				[StencilAttachment release];
			}
		}
	
		// update hash for the depth/stencil buffer & sample count
		PipelineDesc.SetHashValue(Offset_DepthFormat, NumBits_DepthFormat, DepthFormatKey);
		PipelineDesc.SetHashValue(Offset_StencilFormat, NumBits_StencilFormat, StencilFormatKey);
		PipelineDesc.SetHashValue(Offset_SampleCount, NumBits_SampleCount, PipelineDesc.SampleCount);
	
		// commit pending commands on the old render target
		if(CommandEncoder.IsRenderCommandEncoderActive())
		{
			CommandEncoder.EndEncoding();
		}
	
		// make a new render context to use to render to the framebuffer
		CommandEncoder.BeginRenderCommandEncoding(RenderPass);
		
		// Reset any existing state as that must be fully reinitialised by the caller.
		DepthStencilState.SafeRelease();
		RasterizerState.SafeRelease();
		BlendState.SafeRelease();
		BoundShaderState.SafeRelease();
	}
}

void FMetalStateCache::SetHasValidRenderTarget(bool InHasValidRenderTarget)
{
	bHasValidRenderTarget = InHasValidRenderTarget;
}

void FMetalStateCache::SetViewport(const MTLViewport& InViewport)
{
	Viewport = InViewport;
}

void FMetalStateCache::SetVertexBuffer(uint32 const Index, id<MTLBuffer> Buffer, uint32 const Stride, uint32 const Offset)
{
	check(Index < MaxMetalStreams);
	VertexBuffers[Index] = Buffer;
	VertexStrides[Index] = Buffer ? Stride : 0;
	if (Buffer != NULL)
	{
		CommandEncoder.SetShaderBuffer(SF_Vertex, Buffer, Offset, Index);
	}
}

#if PLATFORM_MAC
void FMetalStateCache::SetCurrentLayer(CAMetalLayer* NewLayer)
{
	check(NewLayer);
	CurrentLayer = NewLayer;
	check(CurrentLayer);
}

void FMetalStateCache::SetPrimitiveTopology(MTLPrimitiveTopologyClass PrimitiveType)
{
	PipelineDesc.SetHashValue(Offset_PrimitiveTopology, NumBits_PrimitiveTopology, PrimitiveType);
	PipelineDesc.PipelineDescriptor.inputPrimitiveTopology = PrimitiveType;
}
#endif

void FMetalStateCache::ConditionalSwitchToRender(void)
{
	// were we in blit or compute mode?
	if(CommandEncoder.IsBlitCommandEncoderActive() || CommandEncoder.IsComputeCommandEncoderActive())
	{
		// stop the current encoding and cleanup
		CommandEncoder.EndEncoding();
	}
	
	if (!CommandEncoder.IsRenderCommandEncoderActive())
	{
		// Force a rebind of the render encoder state on the next SetRenderTarget call.
		SetHasValidRenderTarget(false);
	}	
	// we can't start graphics encoding until a new SetRenderTargetsInfo is called because it needs the whole render pass
	// we could cache the render pass if we want to support going back to previous render targets
	// we will catch it by the fact that CommandEncoder will be nil until we call SetRenderTargetsInfo	
}

void FMetalStateCache::ConditionalSwitchToCompute(void)
{
	// if we were in rendering or blit mode, stop the encoding and start compute
	if (CommandEncoder.IsRenderCommandEncoderActive() || CommandEncoder.IsBlitCommandEncoderActive())
	{
		// stop encoding graphics and clean up
		CommandEncoder.EndEncoding();
	}
	if(!CommandEncoder.IsComputeCommandEncoderActive())
	{
		// Clear any previous compute shader
		ComputeShader.SafeRelease();
		
		// start encoding for compute
		CommandEncoder.BeginComputeCommandEncoding();
	}
}

void FMetalStateCache::ConditionalSwitchToBlit(void)
{
	// if we were in rendering or compute mode, stop the encoding and start compute
	if (CommandEncoder.IsRenderCommandEncoderActive() || CommandEncoder.IsComputeCommandEncoderActive())
	{
		// stop encoding graphics and clean up
		CommandEncoder.EndEncoding();
	}
	if(!CommandEncoder.IsBlitCommandEncoderActive())
	{
		// start encoding for compute
		CommandEncoder.BeginBlitCommandEncoding();
	}
}

void FMetalStateCache::ResetCurrentDrawable()
{
	if (CurrentDrawable)
	{
		[CurrentDrawable release];
		CurrentDrawable = nil;
	}
}

void FMetalStateCache::ConditionalUpdateBackBuffer(FMetalSurface& Surface)
{
	// are we setting the back buffer? if so, make sure we have the drawable
//	if (&Surface == &BackBuffer->Surface) // @todo zebra &Surface == &CurrentViewport->GetBackBuffer()->Surface, but perhaps Surface.Texture == nil is enough?
	{
		// update the back buffer texture the first time used this frame
		if (Surface.Texture == nil)
		{
			SCOPE_CYCLE_COUNTER(STAT_MetalMakeDrawableTime);

			uint32 IdleStart = FPlatformTime::Cycles();

			if (CurrentDrawable == nil)
			{
				// make a drawable object for this frame
#if PLATFORM_IOS
				CurrentDrawable = [[[IOSAppDelegate GetDelegate].IOSView MakeDrawable] retain];
#else // @todo zebra
				check(CurrentLayer);
				CurrentDrawable = CurrentLayer ? [[CurrentLayer nextDrawable] retain] : nil;
				//check(CurrentDrawable);
#endif
			}

			GRenderThreadIdle[ERenderThreadIdleTypes::WaitingForGPUPresent] += FPlatformTime::Cycles() - IdleStart;
			GRenderThreadNumIdle[ERenderThreadIdleTypes::WaitingForGPUPresent]++;

			// set the texture into the backbuffer
            if (CurrentDrawable != nil)
            {
                Surface.Texture = CurrentDrawable.texture;
            }
            else
            {
                Surface.Texture = nil;
            }
		}
	}
}

bool FMetalStateCache::NeedsToSetRenderTarget(const FRHISetRenderTargetsInfo& InRenderTargetsInfo) const
{
	// see if our new Info matches our previous Info
	
	// basic checks
	bool bAllChecksPassed = GetHasValidRenderTarget() && CommandEncoder.IsRenderCommandEncoderActive() && InRenderTargetsInfo.NumColorRenderTargets == RenderTargetsInfo.NumColorRenderTargets &&
		// handle the case where going from backbuffer + depth -> backbuffer + null, no need to reset RT and do a store/load
		(InRenderTargetsInfo.DepthStencilRenderTarget.Texture == RenderTargetsInfo.DepthStencilRenderTarget.Texture || InRenderTargetsInfo.DepthStencilRenderTarget.Texture == nullptr);

	// now check each color target if the basic tests passe
	if (bAllChecksPassed)
	{
		for (int32 RenderTargetIndex = 0; RenderTargetIndex < InRenderTargetsInfo.NumColorRenderTargets; RenderTargetIndex++)
		{
			const FRHIRenderTargetView& RenderTargetView = InRenderTargetsInfo.ColorRenderTarget[RenderTargetIndex];
			const FRHIRenderTargetView& PreviousRenderTargetView = RenderTargetsInfo.ColorRenderTarget[RenderTargetIndex];

			// handle simple case of switching textures or mip/slice
			if (RenderTargetView.Texture != PreviousRenderTargetView.Texture ||
				RenderTargetView.MipIndex != PreviousRenderTargetView.MipIndex ||
				RenderTargetView.ArraySliceIndex != PreviousRenderTargetView.ArraySliceIndex)
			{
				bAllChecksPassed = false;
				break;
			}
			
			// it's non-trivial when we need to switch based on load/store action:
			// LoadAction - it only matters what we are switching to in the new one
			//    If we switch to Load, no need to switch as we can re-use what we already have
			//    If we switch to Clear, we have to always switch to a new RT to force the clear
			//    If we switch to DontCare, there's definitely no need to switch
			if (RenderTargetView.LoadAction == ERenderTargetLoadAction::EClear)
			{
				bAllChecksPassed = false;
				break;
			}
			// StoreAction - this matters what the previous one was **In Spirit**
			//    If we come from Store, we need to switch to a new RT to force the store
			//    If we come from DontCare, then there's no need to switch
			//    @todo metal: However, we basically only use Store now, and don't
			//        care about intermediate results, only final, so we don't currently check the value
//			if (PreviousRenderTargetView.StoreAction == ERenderTTargetStoreAction::EStore)
//			{
//				bAllChecksPassed = false;
//				break;
//			}
		}
	}

	// if we are setting them to nothing, then this is probably end of frame, and we can't make a framebuffer
	// with nothng, so just abort this (only need to check on single MRT case)
	if (InRenderTargetsInfo.NumColorRenderTargets == 1 && InRenderTargetsInfo.ColorRenderTarget[0].Texture == nullptr &&
		InRenderTargetsInfo.DepthStencilRenderTarget.Texture == nullptr)
	{
		bAllChecksPassed = true;
	}

	return bAllChecksPassed == false;
}

