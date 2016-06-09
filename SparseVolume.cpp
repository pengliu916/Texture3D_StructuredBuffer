#include "stdafx.h"
#include "SparseVolume.h"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace std;

#define frand() ((float)rand()/RAND_MAX)

SparseVolume::SparseVolume()
	:m_BgThread()
{
	UpdatePerCallData( m_PerCallCBData, XMFLOAT3( m_WidthInUse, m_HeightInUse, m_DepthInUse ),
		m_VoxelSizeInUse, XMFLOAT2( m_MinDensity, m_MaxDensity ), m_VoxelBrickRatio, m_NumMetaBalls );
	for (int i = 0; i < MAX_BALLS; ++i)
		AddBall();
}

SparseVolume::~SparseVolume()
{
	if (m_BgThread.joinable()) m_BgThread.join();
}

void SparseVolume::OnCreateResource()
{
	HRESULT hr;
	ASSERT( Graphics::g_device );

	// Feature support checking
	D3D12_FEATURE_DATA_D3D12_OPTIONS FeatureData = {};
	V( Graphics::g_device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS, &FeatureData, sizeof( FeatureData ) ) );
	if (SUCCEEDED( hr ))
	{
		if (FeatureData.TypedUAVLoadAdditionalFormats)
		{
			D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport = {DXGI_FORMAT_R8G8B8A8_UINT,D3D12_FORMAT_SUPPORT1_NONE,D3D12_FORMAT_SUPPORT2_NONE};
			V( Graphics::g_device->CheckFeatureSupport( D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport, sizeof( FormatSupport ) ) );
			if (FAILED( hr )) PRINTERROR( "Checking Feature Support Failed" );
			if ((FormatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0)
			{
				m_TypeLoadSupported = true; PRINTINFO( "DXGI_FORMAT_R8G8B8A8_UINT typed load is supported" );
			}
			else
				PRINTWARN( "DXGI_FORMAT_R8G8B8A8_UINT typed load is not supported" );
		}
		else
			PRINTWARN( "TypedUAVLoadAdditionalFormats load is not supported" );
	}

	// Compile Shaders
	ComPtr<ID3DBlob> VolumeUpdate_CS[kNumBufferType];
	ComPtr<ID3DBlob> Cube_VS;
	ComPtr<ID3DBlob> Raycast_PS[kNumBufferType];

	uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	D3D_SHADER_MACRO macro[] =
	{
		{"__hlsl",				"1"},
		{"COMPUTE_SHADER",		"0"},
		{"CUBE_VS",				"0"},
		{"RAYCAST_PS",			"0"},
		{"TYPED_UAV",			"0"},
		{"STRUCT_UAV",			"0"},
		{"TEX3D_UAV",			"0"},
		{nullptr,				nullptr}
	};

	macro[2].Definition = "1"; // CUBE_VS
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SparseVolume.hlsl" ) ).c_str(),
		macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs_cube_main", "vs_5_1", compileFlags, 0, &Cube_VS ) );
	macro[2].Definition = "0"; // CUBE_VS

	uint DefIdx;
	for (int i = 0; i < kNumBufferType; ++i)
	{
		switch ((BufferType)i)
		{
		case kStructuredBuffer: DefIdx = 5; break;
		case kTypedBuffer: DefIdx = 4; break;
		case k3DTexBuffer: DefIdx = 6; break;
		}
		macro[DefIdx].Definition = "1";
		macro[1].Definition = "1"; // COMPUTE_SHADER
		V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SparseVolume.hlsl" ) ).c_str(),
			macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_volumeupdate_main", "cs_5_1", compileFlags, 0, &VolumeUpdate_CS[i] ) );
		macro[1].Definition = "0"; // COMPUTE_SHADER

		macro[3].Definition = "1"; // RAYCAST_PS
		V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SparseVolume.hlsl" ) ).c_str(),
			macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_raycast_main", "ps_5_1", compileFlags, 0, &Raycast_PS[i] ) );
		macro[3].Definition = "0"; // RAYCAST_PS
		macro[DefIdx].Definition = "0";
	}
	// Create Rootsignature
	m_RootSignature.Reset( 4, 1 );
	m_RootSignature.InitStaticSampler( 0, Graphics::g_SamplerLinearClampDesc );
	m_RootSignature[0].InitAsConstantBuffer( 0 );
	m_RootSignature[1].InitAsConstantBuffer( 1 );
	m_RootSignature[2].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1 );
	m_RootSignature[3].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1 );
	m_RootSignature.Finalize( D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS );

	// Define the vertex input layout.
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	for (int i = 0; i < kNumBufferType; ++i)
	{
		m_GfxRenderPSO[i][0].SetRootSignature( m_RootSignature );
		m_GfxRenderPSO[i][0].SetInputLayout( _countof( inputElementDescs ), inputElementDescs );
		m_GfxRenderPSO[i][0].SetRasterizerState( Graphics::g_RasterizerDefault );
		m_GfxRenderPSO[i][0].SetBlendState( Graphics::g_BlendDisable );
		m_GfxRenderPSO[i][0].SetDepthStencilState( Graphics::g_DepthStateReadWrite );
		m_GfxRenderPSO[i][0].SetSampleMask( UINT_MAX );
		m_GfxRenderPSO[i][0].SetPrimitiveTopologyType( D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE );
		DXGI_FORMAT ColorFormat = Graphics::g_SceneColorBuffer.GetFormat();
		DXGI_FORMAT DepthFormat = Graphics::g_SceneDepthBuffer.GetFormat();
		m_GfxRenderPSO[i][0].SetRenderTargetFormats( 1, &ColorFormat, DepthFormat );
		m_GfxRenderPSO[i][0].SetVertexShader( Cube_VS->GetBufferPointer(), Cube_VS->GetBufferSize() );
		m_GfxRenderPSO[i][0].SetPixelShader( Raycast_PS[i]->GetBufferPointer(), Raycast_PS[i]->GetBufferSize() );
		m_GfxRenderPSO[i][0].Finalize();

		m_CptUpdatePSO[i][0].SetRootSignature( m_RootSignature );
		m_CptUpdatePSO[i][0].SetComputeShader( VolumeUpdate_CS[i]->GetBufferPointer(), VolumeUpdate_CS[i]->GetBufferSize() );
		m_CptUpdatePSO[i][0].Finalize();
	}
	// Define the geometry for a triangle.
	XMFLOAT3 cubeVertices[] =
	{
		{XMFLOAT3( -0.5f, -0.5f, -0.5f )},
		{XMFLOAT3( -0.5f, -0.5f,  0.5f )},
		{XMFLOAT3( -0.5f,  0.5f, -0.5f )},
		{XMFLOAT3( -0.5f,  0.5f,  0.5f )},
		{XMFLOAT3( 0.5f, -0.5f, -0.5f )},
		{XMFLOAT3( 0.5f, -0.5f,  0.5f )},
		{XMFLOAT3( 0.5f,  0.5f, -0.5f )},
		{XMFLOAT3( 0.5f,  0.5f,  0.5f )},
	};

	const uint32_t vertexBufferSize = sizeof( cubeVertices );
	m_VertexBuffer.Create( L"Vertex Buffer", ARRAYSIZE( cubeVertices ), sizeof( XMFLOAT3 ), (void*)cubeVertices );

	uint16_t cubeIndices[] =
	{
		0,2,1, 1,2,3,  4,5,6, 5,7,6,  0,1,5, 0,5,4,  2,6,7, 2,7,3,  0,4,6, 0,6,2,  1,3,7, 1,7,5,
	};

	m_IndexBuffer.Create( L"Index Buffer", ARRAYSIZE( cubeIndices ), sizeof( uint16_t ), (void*)cubeIndices );

	// Create Buffer Resources
	uint32_t volumeBufferElementCount = m_DepthInUse*m_HeightInUse*m_WidthInUse;
	switch (m_BufferTypeInUse)
	{
	case SparseVolume::kStructuredBuffer:
		m_StructuredVolBuf[m_OnStageIdx].Create( L"Struct Volume Buffer", volumeBufferElementCount, 4 * sizeof( uint32_t ) );
		break;
	case SparseVolume::kTypedBuffer:
		m_TypedBuffer[m_OnStageIdx].Create( L"Typed Volume Buffer", volumeBufferElementCount, 4 * sizeof( uint32_t ) );
		break;
	case SparseVolume::k3DTexBuffer:
		m_VolumeTexBuf[m_OnStageIdx].Create( L"Texture3D Volume Buffer", m_WidthInUse, m_HeightInUse, m_DepthInUse, DXGI_FORMAT_R32G32B32A32_FLOAT );
		break;
	}

	// Create the initial volume 
	m_atomicState.store( kNewBufferCooking, memory_order_release );
	if (m_BgThread.joinable()) m_BgThread.join();
	m_BgThread = std::thread( &SparseVolume::CookVolume, this, m_WidthInUse, m_HeightInUse, m_DepthInUse,
		m_BufferTypeInUse, m_BufferTypeInUse );
}

void SparseVolume::OnRender( CommandContext& cmdContext, DirectX::XMMATRIX wvp, DirectX::XMMATRIX mView, DirectX::XMFLOAT4 eyePos )
{
	switch (m_atomicState.load( memory_order_acquire ))
	{
	case kNewBufferReady:
		m_BufferTypeInUse = m_NewBufferType;
		m_WidthInUse = m_NewWidth;
		m_HeightInUse = m_NewHeight;
		m_DepthInUse = m_NewDepth;
		m_OnStageIdx = 1 - m_OnStageIdx;
		m_FenceValue = Graphics::g_stats.lastFrameEndFence;
		m_atomicState.store( kRetiringOldBuffer, memory_order_release );
		break;
	case kRetiringOldBuffer:
		if (Graphics::g_cmdListMngr.IsFenceComplete( m_FenceValue ))
			m_atomicState.store( kOldBufferRetired, memory_order_release );
		break;
	}
	UpdatePerFrameData( wvp, mView, eyePos );

#define BindVolumeResource(ctx,volResource,state) \
	ctx.TransitionResource(volResource,state); \
	ctx.SetDynamicDescriptors(2,0,1,&volResource.GetUAV()); \
	ctx.SetDynamicDescriptors(3,0,1,&volResource.GetSRV());

	if (m_Animated)
	{
		ComputeContext& cptContext = cmdContext.GetComputeContext();
		{
			GPU_PROFILE( cptContext, L"Volume Updating" );
			cptContext.SetRootSignature( m_RootSignature );
			cptContext.SetPipelineState( m_CptUpdatePSO[m_BufferTypeInUse][0] );
			cptContext.SetDynamicConstantBufferView( 0, sizeof( m_PerFrameCBData ), (void*)&m_PerFrameCBData );
			cptContext.SetDynamicConstantBufferView( 1, sizeof( m_PerCallCBData ), (void*)&m_PerCallCBData );
			switch (m_BufferTypeInUse)
			{
			case kStructuredBuffer:
				BindVolumeResource( cptContext, m_StructuredVolBuf[m_OnStageIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
				break;
			case kTypedBuffer:
				BindVolumeResource( cptContext, m_TypedBuffer[m_OnStageIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
				break;
			case k3DTexBuffer:
				BindVolumeResource( cptContext, m_VolumeTexBuf[m_OnStageIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
				break;
			}
			cptContext.Dispatch( m_WidthInUse / THREAD_X, m_HeightInUse / THREAD_Y, m_DepthInUse / THREAD_Z );
		}
	}
	GraphicsContext& gfxContext = cmdContext.GetGraphicsContext();
	{
		GPU_PROFILE( gfxContext, L"Rendering" );
		gfxContext.SetRootSignature( m_RootSignature );
		gfxContext.SetPipelineState( m_GfxRenderPSO[m_BufferTypeInUse][0] );
		gfxContext.SetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		gfxContext.SetDynamicConstantBufferView( 0, sizeof( m_PerFrameCBData ), (void*)&m_PerFrameCBData );
		gfxContext.SetDynamicConstantBufferView( 1, sizeof( m_PerCallCBData ), (void*)&m_PerCallCBData );
		switch (m_BufferTypeInUse)
		{
		case kStructuredBuffer:
			BindVolumeResource( gfxContext, m_StructuredVolBuf[m_OnStageIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			break;
		case kTypedBuffer:
			BindVolumeResource( gfxContext, m_TypedBuffer[m_OnStageIdx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			break;
		case k3DTexBuffer:
			BindVolumeResource( gfxContext, m_VolumeTexBuf[m_OnStageIdx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
			break;
		}
		gfxContext.SetRenderTargets( 1, &Graphics::g_SceneColorBuffer, &Graphics::g_SceneDepthBuffer );
		gfxContext.SetViewport( Graphics::g_DisplayPlaneViewPort );
		gfxContext.SetScisor( Graphics::g_DisplayPlaneScissorRect );
		gfxContext.SetVertexBuffer( 0, m_VertexBuffer.VertexBufferView() );
		gfxContext.SetIndexBuffer( m_IndexBuffer.IndexBufferView() );
		gfxContext.DrawIndexed( 36 );
	}
#undef BindVolumeResource
}

void SparseVolume::RenderGui()
{
	static bool showPenal = true;
	if (ImGui::CollapsingHeader( "Sparse Volume", 0, true, true ))
	{
		ImGui::Text( "Buffer Settings:" );
		static int uBufferChoice = m_BufferTypeInUse;
		ImGui::RadioButton( "Use Typed Buffer", &uBufferChoice, kTypedBuffer );
		ImGui::RadioButton( "Use Structured Buffer", &uBufferChoice, kStructuredBuffer );
		ImGui::RadioButton( "Use Texture3D Buffer", &uBufferChoice, k3DTexBuffer );
		if (uBufferChoice != m_BufferTypeInUse && m_atomicState.load( memory_order_acquire ) == kNormal)
		{
			m_atomicState.store( kNewBufferCooking, memory_order_release );
			m_NewBufferType = (BufferType)uBufferChoice;
			if (m_BgThread.joinable()) m_BgThread.join();
			m_BgThread = std::thread( &SparseVolume::CookVolume, this, m_WidthInUse, m_HeightInUse, m_DepthInUse,
				m_NewBufferType, m_BufferTypeInUse );
		}
		ImGui::Separator();

		ImGui::Text( "Volume Size Settings:" );
		static int uiVolumeWide = m_WidthInUse;
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::Text( "X:" ); ImGui::SameLine();
		ImGui::RadioButton( "128##X", &uiVolumeWide, 128 ); ImGui::SameLine();
		ImGui::RadioButton( "256##X", &uiVolumeWide, 256 ); ImGui::SameLine();
		ImGui::RadioButton( "384##X", &uiVolumeWide, 384 );
		if (uiVolumeWide != m_WidthInUse && m_atomicState.load( memory_order_acquire ) == kNormal)
		{
			m_atomicState.store( kNewBufferCooking, memory_order_release );
			m_NewWidth = uiVolumeWide;
			if (m_BgThread.joinable()) m_BgThread.join();
			m_BgThread = std::thread( &SparseVolume::CookVolume, this, m_NewWidth, m_HeightInUse, m_DepthInUse,
				m_BufferTypeInUse, m_BufferTypeInUse );
		}

		static int uiVolumeHeight = m_HeightInUse;
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::Text( "Y:" ); ImGui::SameLine();
		ImGui::RadioButton( "128##Y", &uiVolumeHeight, 128 ); ImGui::SameLine();
		ImGui::RadioButton( "256##Y", &uiVolumeHeight, 256 ); ImGui::SameLine();
		ImGui::RadioButton( "384##Y", &uiVolumeHeight, 384 );
		if (uiVolumeHeight != m_HeightInUse && m_atomicState.load( memory_order_acquire ) == kNormal)
		{
			m_atomicState.store( kNewBufferCooking, memory_order_release );
			m_NewHeight = uiVolumeHeight;
			if (m_BgThread.joinable()) m_BgThread.join();
			m_BgThread = std::thread( &SparseVolume::CookVolume, this, m_WidthInUse, m_NewHeight, m_DepthInUse,
				m_BufferTypeInUse, m_BufferTypeInUse );
		}

		static int uiVolumeDepth = m_DepthInUse;
		ImGui::AlignFirstTextHeightToWidgets();
		ImGui::Text( "Z:" ); ImGui::SameLine();
		ImGui::RadioButton( "128##Z", &uiVolumeDepth, 128 ); ImGui::SameLine();
		ImGui::RadioButton( "256##Z", &uiVolumeDepth, 256 ); ImGui::SameLine();
		ImGui::RadioButton( "384##Z", &uiVolumeDepth, 384 );
		if (uiVolumeDepth != m_DepthInUse && m_atomicState.load( memory_order_acquire ) == kNormal)
		{
			m_atomicState.store( kNewBufferCooking, memory_order_release );
			m_NewDepth = uiVolumeDepth;
			if (m_BgThread.joinable()) m_BgThread.join();
			m_BgThread = std::thread( &SparseVolume::CookVolume, this, m_WidthInUse, m_HeightInUse, m_NewDepth,
				m_BufferTypeInUse, m_BufferTypeInUse );
		}
	}
}

void SparseVolume::UpdatePerCallData( PerCallDataCB& DataCB, DirectX::XMFLOAT3 VoxRes, float VoxSize,
	DirectX::XMFLOAT2 MinMaxDensity, uint VoxBrickRatio, uint NumOfBalls )
{
	DataCB.f3VoxelReso = VoxRes;
	DataCB.fVoxelSize = VoxSize;
	DataCB.f3InvVolSize = XMFLOAT3( 1.f / (VoxRes.x*VoxSize), 1.f / (VoxRes.y*VoxSize), 1.f / (VoxRes.z*VoxSize) );
	DataCB.f2MinMaxDensity = MinMaxDensity;
	DataCB.uVoxelBrickRatio = VoxBrickRatio;
	DataCB.f3BoxMin = XMFLOAT3( -0.5f*VoxRes.x*VoxSize, -0.5f*VoxRes.y*VoxSize, -0.5f*VoxRes.z*VoxSize );
	DataCB.uNumOfBalls = NumOfBalls;
	DataCB.f3BoxMax = XMFLOAT3( 0.5f*VoxRes.x*VoxSize, 0.5f*VoxRes.y*VoxSize, 0.5f*VoxRes.z*VoxSize );
}

void SparseVolume::UpdatePerFrameData( DirectX::XMMATRIX wvp, DirectX::XMMATRIX mView, DirectX::XMFLOAT4 eyePos )
{
	m_PerFrameCBData.mWorldViewProj = wvp;
	m_PerFrameCBData.mView = mView;
	m_PerFrameCBData.f4ViewPos = eyePos;
	if (m_Animated) {
		m_AnimateTime += Core::g_deltaTime;
		for (int i = 0; i < m_PerCallCBData.uNumOfBalls; i++) {
			Ball ball = m_vecBalls[i];
			m_PerFrameCBData.f4Balls[i].x = ball.fOribtRadius * (float)cosf( m_AnimateTime * ball.fOribtSpeed + ball.fOribtStartPhase );
			m_PerFrameCBData.f4Balls[i].y = ball.fOribtRadius * (float)sinf( m_AnimateTime * ball.fOribtSpeed + ball.fOribtStartPhase );
			m_PerFrameCBData.f4Balls[i].z = 0.3f * ball.fOribtRadius * (float)sinf( 2.f * m_AnimateTime * ball.fOribtSpeed + ball.fOribtStartPhase );
			m_PerFrameCBData.f4Balls[i].w = ball.fPower;
			m_PerFrameCBData.f4BallsCol[i] = ball.f4Color;
		}
	}
}

void SparseVolume::CookVolume( uint32_t Width, uint32_t Height, uint32_t Depth, BufferType BufType, BufferType PreBufType )
{
	uint32_t BufferElmCount = Width * Height * Depth;
	uint32_t BufferSize = Width * Height * Depth * 4 * sizeof( uint32_t );

	if (BufType == kTypedBuffer) m_TypedBuffer[1 - m_OnStageIdx].Create( L"Typed Volume Buffer", BufferElmCount, 4 * sizeof( uint32_t ) );
	if (BufType == kStructuredBuffer) m_StructuredVolBuf[1 - m_OnStageIdx].Create( L"Struct Volume Buffer", BufferElmCount, 4 * sizeof( uint32_t ) );
	if (BufType == k3DTexBuffer) m_VolumeTexBuf[1 - m_OnStageIdx].Create( L"Texture3D Volume Buffer", Width, Height, Depth, DXGI_FORMAT_R32G32B32A32_FLOAT );

	UpdatePerCallData( m_PerCallCBData, XMFLOAT3( Width, Height, Depth ), m_VoxelSizeInUse, XMFLOAT2( m_MinDensity, m_MaxDensity ), m_VoxelBrickRatio, m_NumMetaBalls );

	m_NewBufferType = BufType;
	m_NewWidth = Width;
	m_NewHeight = Height;
	m_NewDepth = Depth;

	m_atomicState.store( kNewBufferReady, memory_order_release );

	while (m_atomicState.load( memory_order_acquire ) != kOldBufferRetired)
	{
		this_thread::yield();
	}

	if (PreBufType == kTypedBuffer) m_TypedBuffer[1 - m_OnStageIdx].Destroy();
	if (PreBufType == kStructuredBuffer) m_StructuredVolBuf[1 - m_OnStageIdx].Destroy();
	if (PreBufType == k3DTexBuffer) m_VolumeTexBuf[1 - m_OnStageIdx].Destroy();

	m_BufferTypeInUse = BufType;
	m_WidthInUse = m_NewWidth;
	m_HeightInUse = m_NewHeight;
	m_DepthInUse = m_NewDepth;

	m_atomicState.store( kNormal, memory_order_release );
}

void SparseVolume::AddBall()
{
	Ball ball;
	float r = (0.6f * frand() + 0.7f) * m_PerCallCBData.f3VoxelReso.x * m_PerCallCBData.fVoxelSize * 0.05f;
	ball.fPower = r * r;
	ball.fOribtRadius = m_PerCallCBData.f3VoxelReso.x * m_PerCallCBData.fVoxelSize * (0.3f + (frand() - 0.3f) * 0.2f);

	if (ball.fOribtRadius + r > 0.45f * m_PerCallCBData.f3VoxelReso.x * m_PerCallCBData.fVoxelSize)
	{
		r = 0.45f * m_PerCallCBData.f3VoxelReso.x * m_PerCallCBData.fVoxelSize - ball.fOribtRadius;
		ball.fPower = r * r;
	}
	float speedF = 6.f * (frand() - 0.5f);
	if (abs( speedF ) < 1.f) speedF = (speedF > 0.f ? 1.f : -1.f) * 1.f;
	ball.fOribtSpeed = 1.0f / ball.fPower * 0.0005f * speedF;
	ball.fOribtStartPhase = frand() * 6.28f;

	float alpha = frand() * 6.28f;
	float beta = frand() * 6.28f;
	float gamma = frand() * 6.28f;

	XMMATRIX rMatrix = XMMatrixRotationRollPitchYaw( alpha, beta, gamma );
	XMVECTOR colVect = XMVector3TransformNormal( XMLoadFloat3( &XMFLOAT3( 1, 0, 0 ) ), rMatrix );
	XMFLOAT4 col;
	XMStoreFloat4( &col, colVect );
	col.x = abs( col.x );
	col.y = abs( col.y );
	col.z = abs( col.z );

	ball.f4Color = col;

	if (m_vecBalls.size() < MAX_BALLS) m_vecBalls.push_back( ball );
}