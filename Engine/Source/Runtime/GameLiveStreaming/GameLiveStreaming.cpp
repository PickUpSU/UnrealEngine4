// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#include "GameLiveStreamingModule.h"
#include "GameLiveStreaming.h"
#include "Runtime/Core/Public/Features/IModularFeatures.h"
#include "Runtime/Engine/Public/Features/ILiveStreamingService.h"
#include "Slate.h"
#include "ScreenRendering.h"
#include "RenderCore.h"
#include "RHIStaticStates.h"

IMPLEMENT_MODULE( FGameLiveStreaming, GameLiveStreaming );

#define LOCTEXT_NAMESPACE "GameLiveStreaming"

FGameLiveStreaming::FGameLiveStreaming()
{
	LiveStreamer = nullptr;
	ReadbackTextureIndex = 0;
	ReadbackBufferIndex = 0;
	ReadbackBuffers[0] = nullptr;
	ReadbackBuffers[1] = nullptr;
	bDrawSimpleWebCamVideo = true;

	// Setup some console commands
	{
		static FAutoConsoleCommand BroadcastStartCommand
		(
			TEXT( "Broadcast.Start" ),
			TEXT( "Starts broadcasting game video and audio using a live internet streaming service\n" )
			TEXT( "\n" )
			TEXT( "Optional parameters:\n" )
			TEXT( "     FrameRate=<int32>                   Frame rate to stream video when broadcasting\n")
			TEXT( "     ScreenScaling=<float>               How much to scale the broadcast video resolution down\n" )
			TEXT( "     bEnableWebCam=<bool>                Enables video from your web camera to be captured and displayed\n" )
			TEXT( "     DesiredWebCamWidth=<int32>          Horizontal resolution for web camera capture\n" )
			TEXT( "     DesiredWebCamHeight=<int32>         Veritcal resolution for web camera capture\n" )
			TEXT( "     bCaptureAudioFromComputer=<bool>    Enables capturing sound that is played back through your PC\n" )
			TEXT( "     bCaptureAudioFromMicrophone=<bool>  Enables capturing sound from your default microphone\n" )
			TEXT( "     bDrawSimpleWebCamVideo=<bool>       Draws a simple web cam video on top of the viewport\n" ),
			FConsoleCommandWithArgsDelegate::CreateStatic( 
				[]( const TArray< FString >& Args )
				{
					FGameBroadcastConfig Config;
					for( FString Arg : Args )
					{
						FParse::Value( *Arg, TEXT( "FrameRate="), Config.FrameRate );
						FParse::Value( *Arg, TEXT( "ScreenScaling="), Config.ScreenScaling );
						FParse::Bool( *Arg, TEXT( "bEnableWebCam="), Config.bEnableWebCam );
						FParse::Value( *Arg, TEXT( "DesiredWebCamWidth="), Config.DesiredWebCamWidth );
						FParse::Value( *Arg, TEXT( "DesiredWebCamHeight="), Config.DesiredWebCamHeight );
						FParse::Bool( *Arg, TEXT( "bCaptureAudioFromComputer="), Config.bCaptureAudioFromComputer );
						FParse::Bool( *Arg, TEXT( "bCaptureAudioFromMicrophone="), Config.bCaptureAudioFromMicrophone );
						FParse::Bool( *Arg, TEXT( "bDrawSimpleWebCamVideo=" ), Config.bDrawSimpleWebCamVideo );
					}
					IGameLiveStreaming::Get().StartBroadcastingGame( Config );
				} )
		);

		static FAutoConsoleCommand BroadcastStopCommand
		(
			TEXT( "Broadcast.Stop" ),
			TEXT( "Stops broadcasting game video" ),
			FConsoleCommandDelegate::CreateStatic( 
				[]
				{
					IGameLiveStreaming::Get().StopBroadcastingGame();
				} )
		);
	}
}


void FGameLiveStreaming::StartupModule()
{
}


void FGameLiveStreaming::ShutdownModule()
{
	// Stop broadcasting at shutdown
	StopBroadcastingGame();
}



bool FGameLiveStreaming::IsBroadcastingGame() const
{
	bool bAnythingBroadcasting = false;
	if( LiveStreamer != nullptr )
	{
		if( LiveStreamer->IsBroadcasting() )
		{
			bAnythingBroadcasting = true;
		}
	}
	return bAnythingBroadcasting;
}


void FGameLiveStreaming::StartBroadcastingGame( const FGameBroadcastConfig& GameBroadcastConfig )
{
	if( !IsBroadcastingGame() )
	{
		if( LiveStreamer != nullptr )
		{
			FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
			SlateRenderer->OnSlateWindowRendered().RemoveAll( this );
			LiveStreamer->OnStatusChanged().RemoveAll( this );
			LiveStreamer = nullptr;
		}

		static const FName LiveStreamingFeatureName( "LiveStreaming" );
		if( IModularFeatures::Get().IsModularFeatureAvailable( LiveStreamingFeatureName ) )
		{
			FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
			SlateRenderer->OnSlateWindowRendered().AddRaw( this, &FGameLiveStreaming::OnSlateWindowRenderedDuringBroadcasting );

			this->bDrawSimpleWebCamVideo = GameBroadcastConfig.bDrawSimpleWebCamVideo;

			// Select a live streaming service
			// @todo livestream: This will interfere with editor live streaming if both are running at the same time!  The editor live 
			// streaming does check to make sure that game isn't already broadcasting, but the game currently doesn't have a good way to
			// do that, besides asking the LiveStreamer itself.
			// @todo livestream: Always using first live streaming plugin we find, but instead this should be a configurable option
			LiveStreamer = static_cast<ILiveStreamingService*>( IModularFeatures::Get().GetModularFeatureImplementation( LiveStreamingFeatureName, 0 ) );

			// Register to find out about status changes
			LiveStreamer->OnStatusChanged().AddRaw( this, &FGameLiveStreaming::BroadcastStatusCallback );

			UGameViewportClient* GameViewportClient = GEngine->GameViewport;
			check( GameViewportClient != nullptr );

			// @todo livestream: What about if viewport size changes while we are still broadcasting?  We need to restart the broadcast!
			FBroadcastConfig BroadcastConfig;
			BroadcastConfig.VideoBufferWidth = GameViewportClient->Viewport->GetSizeXY().X;
			BroadcastConfig.VideoBufferHeight = GameViewportClient->Viewport->GetSizeXY().Y;

			BroadcastConfig.VideoBufferWidth = FPlatformMath::FloorToInt( (float)BroadcastConfig.VideoBufferWidth * GameBroadcastConfig.ScreenScaling );
			BroadcastConfig.VideoBufferHeight = FPlatformMath::FloorToInt( (float)BroadcastConfig.VideoBufferHeight * GameBroadcastConfig.ScreenScaling );

			// Fix up the desired resolution so that it will work with the streaming system.  Some broadcasters require the
			// video buffer to be multiples of specific values, such as 32
			// @todo livestream: This could cause the aspect ratio to be changed and the buffer to be stretched non-uniformly, but usually the aspect only changes slightly
			LiveStreamer->MakeValidVideoBufferResolution( BroadcastConfig.VideoBufferWidth, BroadcastConfig.VideoBufferHeight );

			// Setup readback buffer textures
			{
				for( int32 TextureIndex = 0; TextureIndex < 2; ++TextureIndex )
				{
					FRHIResourceCreateInfo CreateInfo;
					ReadbackTextures[ TextureIndex ] = RHICreateTexture2D(
						BroadcastConfig.VideoBufferWidth,
						BroadcastConfig.VideoBufferHeight,
						PF_B8G8R8A8,
						1,
						1,
						TexCreate_CPUReadback,
						CreateInfo
						);
				}
	
				ReadbackTextureIndex = 0;

				ReadbackBuffers[0] = nullptr;
				ReadbackBuffers[1] = nullptr;
				ReadbackBufferIndex = 0;
			}

			BroadcastConfig.FramesPerSecond = GameBroadcastConfig.FrameRate;
			BroadcastConfig.PixelFormat = FBroadcastConfig::EBroadcastPixelFormat::B8G8R8A8;	// Matches viewport backbuffer format
			BroadcastConfig.bCaptureAudioFromComputer = GameBroadcastConfig.bCaptureAudioFromComputer;
			BroadcastConfig.bCaptureAudioFromMicrophone = GameBroadcastConfig.bCaptureAudioFromMicrophone;
			LiveStreamer->StartBroadcasting( BroadcastConfig );

			if( GameBroadcastConfig.bEnableWebCam )
			{
				// @todo livestream: Allow web cam to be started/stopped independently from the broadcast itself, so users can setup their web cam
				FWebCamConfig WebCamConfig;
				WebCamConfig.DesiredWebCamWidth = GameBroadcastConfig.DesiredWebCamWidth;
				WebCamConfig.DesiredWebCamHeight = GameBroadcastConfig.DesiredWebCamHeight;
				LiveStreamer->StartWebCam( WebCamConfig );
			}
		}
	}
}


void FGameLiveStreaming::StopBroadcastingGame()
{
	if( LiveStreamer != nullptr )
	{
		LiveStreamer->OnStatusChanged().RemoveAll( this );

		if( LiveStreamer->IsBroadcasting() )
		{
			LiveStreamer->StopWebCam();
			LiveStreamer->StopBroadcasting();
		}
		if( FSlateApplication::IsInitialized() )	// During shutdown, Slate may have already been destroyed by the time our viewport gets cleaned up
		{
			FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
			SlateRenderer->OnSlateWindowRendered().RemoveAll( this );
		}
		LiveStreamer = nullptr;

		// Cleanup readback buffer textures
		{
			FlushRenderingCommands();

			ReadbackTextures[0].SafeRelease();
			ReadbackTextures[1].SafeRelease();
			ReadbackTextureIndex = 0;
			ReadbackBuffers[0] = nullptr;
			ReadbackBuffers[1] = nullptr;
			ReadbackBufferIndex = 0;
		}
	}
}



void FGameLiveStreaming::OnSlateWindowRenderedDuringBroadcasting( SWindow& SlateWindow, void* ViewportRHIPtr )
{
	// If we're streaming live video/audio, we'll go ahead and push new video frames here
	UGameViewportClient* GameViewportClient = GEngine->GameViewport;
	if( IsBroadcastingGame() && GameViewportClient != nullptr )
	{
		// We only care about our own Slate window
		if( GameViewportClient->GetWindow() == SlateWindow.AsShared() )
		{
			// Check to see if we're streaming live video.  If so, we'll want to push new frames to be broadcast.
			if( LiveStreamer->IsReadyForVideoFrames() )
			{
				// Check to see if there are any video frames ready to push
				BroadcastGameVideoFrame();

				// Start copying next game video frame
				const FViewportRHIRef* ViewportRHI = ( const FViewportRHIRef* )ViewportRHIPtr;
				StartCopyingNextGameVideoFrame( *ViewportRHI );
			}
		}
	}
	else
	{
		FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();

		// No longer broadcasting.  The live streaming service may have been interrupted.
		LiveStreamer->OnStatusChanged().RemoveAll( this );
		LiveStreamer = nullptr;
		SlateRenderer->OnSlateWindowRendered().RemoveAll( this );
	}
}


void FGameLiveStreaming::BroadcastGameVideoFrame()
{
	if( ReadbackBuffers[ ReadbackBufferIndex ] != nullptr )
	{
		// Great, we have a new frame back from the GPU.  Upload the video frame!
		LiveStreamer->PushVideoFrame( (FColor*)ReadbackBuffers[ ReadbackBufferIndex ] );

		// Unmap the buffer now that we've pushed out the frame
		{
			struct FReadbackFromStagingBufferContext
			{
				FGameLiveStreaming* This;
			};
			FReadbackFromStagingBufferContext ReadbackFromStagingBufferContext =
			{
				this
			};
			ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
				ReadbackFromStagingBuffer,
				FReadbackFromStagingBufferContext,Context,ReadbackFromStagingBufferContext,
			{
				RHIUnmapStagingSurface( Context.This->ReadbackTextures[ Context.This->ReadbackTextureIndex ] );
			});
		}								
	}
	else
	{
		// Frame hasn't finished copying from the GPU, so we can't broadcast a frame yet
	}
}


void FGameLiveStreaming::StartCopyingNextGameVideoFrame( const FViewportRHIRef& ViewportRHI )
{
	// Check the video buffer size, in case we were configured to downscale the image
	FBroadcastConfig BroadcastConfig;
	LiveStreamer->QueryBroadcastConfig( BroadcastConfig );

	const FIntPoint ResizeTo( BroadcastConfig.VideoBufferWidth, BroadcastConfig.VideoBufferHeight );

	static const FName RendererModuleName( "Renderer" );
	IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>( RendererModuleName );

	UGameViewportClient* GameViewportClient = GEngine->GameViewport;
	check( GameViewportClient != nullptr );

	struct FCopyVideoFrame
	{
		FViewportRHIRef ViewportRHI;
		IRendererModule* RendererModule;
		FIntPoint ResizeTo;
		FGameLiveStreaming* This;
	};
	FCopyVideoFrame CopyVideoFrame =
	{
		ViewportRHI,
		&RendererModule,
		ResizeTo,
		this
	};

	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		ReadSurfaceCommand,
		FCopyVideoFrame,Context,CopyVideoFrame,
	{
		FPooledRenderTargetDesc OutputDesc(FPooledRenderTargetDesc::Create2DDesc(Context.ResizeTo, PF_B8G8R8A8, TexCreate_None, TexCreate_RenderTargetable, false));
			
		TRefCountPtr<IPooledRenderTarget> ResampleTexturePooledRenderTarget;
		Context.RendererModule->RenderTargetPoolFindFreeElement(OutputDesc, ResampleTexturePooledRenderTarget, TEXT("ResampleTexture"));
		check( ResampleTexturePooledRenderTarget );

		const FSceneRenderTargetItem& DestRenderTarget = ResampleTexturePooledRenderTarget->GetRenderTargetItem();

		RHISetRenderTarget( DestRenderTarget.TargetableTexture, FTextureRHIRef() );
		RHISetViewport(0, 0, 0.0f, Context.ResizeTo.X, Context.ResizeTo.Y, 1.0f);

		RHISetBlendState(TStaticBlendState<>::GetRHI());
		RHISetRasterizerState(TStaticRasterizerState<>::GetRHI());
		RHISetDepthStencilState(TStaticDepthStencilState<false,CF_Always>::GetRHI());

		FRHICommandList& RHICmdList = FRHICommandList::GetNullRef();

		FTexture2DRHIRef ViewportBackBuffer = RHIGetViewportBackBuffer( Context.ViewportRHI );

		TShaderMapRef<FScreenVS> VertexShader(GetGlobalShaderMap());
		TShaderMapRef<FScreenPS> PixelShader(GetGlobalShaderMap());

		static FGlobalBoundShaderState BoundShaderState;
		SetGlobalBoundShaderState(RHICmdList, BoundShaderState, Context.RendererModule->GetFilterVertexDeclaration().VertexDeclarationRHI, *VertexShader, *PixelShader);

		if( Context.ResizeTo != FIntPoint( ViewportBackBuffer->GetSizeX(), ViewportBackBuffer->GetSizeY() ) )
		{
			// We're scaling down the window, so use bilinear filtering
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Bilinear>::GetRHI(), ViewportBackBuffer);
		}
		else
		{
			// Drawing 1:1, so no filtering needed
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Point>::GetRHI(), ViewportBackBuffer);
		}

		Context.RendererModule->DrawRectangle(
			RHICmdList,
			0, 0,		// Dest X, Y
			Context.ResizeTo.X, Context.ResizeTo.Y,	// Dest Width, Height
			0, 0,		// Source U, V
			1, 1,		// Source USize, VSize
			Context.ResizeTo,		// Target buffer size
			FIntPoint(1, 1),		// Source texture size
			*VertexShader,
			EDRF_Default);

		// Asynchronously copy render target from GPU to CPU
		const bool bKeepOriginalSurface = false;
		RHICopyToResolveTarget(
			DestRenderTarget.TargetableTexture,
			Context.This->ReadbackTextures[ Context.This->ReadbackTextureIndex ],
			bKeepOriginalSurface,
			FResolveParams());
	});
							

	// Start mapping the newly-rendered buffer
	{
		struct FReadbackFromStagingBufferContext
		{
			FGameLiveStreaming* This;
		};
		FReadbackFromStagingBufferContext ReadbackFromStagingBufferContext =
		{
			this
		};
		ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
			ReadbackFromStagingBuffer,
			FReadbackFromStagingBufferContext,Context,ReadbackFromStagingBufferContext,
		{
			int32 UnusedWidth = 0;
			int32 UnusedHeight = 0;
			RHIMapStagingSurface( Context.This->ReadbackTextures[ Context.This->ReadbackTextureIndex ], Context.This->ReadbackBuffers[ Context.This->ReadbackBufferIndex ], UnusedWidth, UnusedHeight );

			// Ping pong between readback textures
			Context.This->ReadbackTextureIndex = ( Context.This->ReadbackTextureIndex + 1 ) % 2;
		});
	}

	// Ping pong between readback buffers
	ReadbackBufferIndex = ( ReadbackBufferIndex + 1 ) % 2;
}


void FGameLiveStreaming::DrawSimpleWebCamVideo( UCanvas* Canvas )
{
	if( IsBroadcastingGame() && bDrawSimpleWebCamVideo && LiveStreamer->IsWebCamEnabled() )
	{
		UTexture2D* WebCamTexture = LiveStreamer->GetWebCamTexture();
		if( WebCamTexture != nullptr )
		{
			const float BorderPadding = 6.0f;
			Canvas->Canvas->DrawTile( 
				Canvas->SizeX - WebCamTexture->GetSizeX() - BorderPadding, BorderPadding,	// Top right justify
				WebCamTexture->GetSizeX(), WebCamTexture->GetSizeY(),
				0, 0, 
				1.0f, 1.0f, 
				FLinearColor::White, 
				WebCamTexture->Resource, 
				false );	// Alpha blend?
		}
	}
}


UTexture2D* FGameLiveStreaming::GetWebCamTexture()
{
	UTexture2D* WebCamTexture = nullptr;
	if( IsBroadcastingGame() && bDrawSimpleWebCamVideo && LiveStreamer->IsWebCamEnabled() )
	{
		WebCamTexture = LiveStreamer->GetWebCamTexture();
	}
	return WebCamTexture;
}

	
void FGameLiveStreaming::BroadcastStatusCallback( const FLiveStreamingStatus& Status )
{
	// @todo livestream: Hook this up to provide C++ users and Blueprints with status about the live streaming startup/webcam/shutdown
}



#undef LOCTEXT_NAMESPACE
