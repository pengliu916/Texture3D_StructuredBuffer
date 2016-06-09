#pragma once
#include "SparseVolume_SharedHeader.inl"
class SparseVolume
{
public:
	enum BufferType
	{
		kStructuredBuffer = 0,
		kTypedBuffer = 1,
		k3DTexBuffer = 2,
		kNumBufferType
	};

	enum VolStructType
	{
		kVoxelsOnly = 0,
		kVoxelsAndBricks = 1,
		kNumVolStructType
	};

	enum ResourceState
	{
		kNormal = 0,
		kNewBufferCooking = 1,
		kNewBufferReady = 2,
		kRetiringOldBuffer = 3,
		kOldBufferRetired = 4,
		kNumStates
	};

	struct Ball
	{
		float fPower;					// size of this metaball
		float fOribtRadius;				// radius of orbit
		float fOribtSpeed;				// speed of rotation
		float fOribtStartPhase;			// initial phase
		DirectX::XMFLOAT4 f4Color;		// color
	};

	SparseVolume();
	~SparseVolume();

	void OnCreateResource();
	void OnRender( CommandContext& cmdContext, DirectX::XMMATRIX wvp, DirectX::XMMATRIX mView, DirectX::XMFLOAT4 eyePos );
	void RenderGui();

	void CookVolume( uint32_t Width, uint32_t Height, uint32_t Depth, BufferType BufType, BufferType PreBufType);
	void UpdatePerCallData( PerCallDataCB& DataCB, DirectX::XMFLOAT3 VoxRes, float VoxSize,
		DirectX::XMFLOAT2 MinMaxDensity, uint VoxBrickRatio, uint NumOfBalls );
	void UpdatePerFrameData(DirectX::XMMATRIX wvp, DirectX::XMMATRIX mView, DirectX::XMFLOAT4 eyePos);

	void AddBall();

	// Volume settings currently in use
	BufferType					m_BufferTypeInUse = kStructuredBuffer;
	VolStructType				m_VolStructTypeInUse = kVoxelsOnly;
	uint32_t					m_WidthInUse = 256;
	uint32_t					m_HeightInUse = 256;
	uint32_t					m_DepthInUse = 256;
	float						m_VoxelSizeInUse = 0.01f;
	float						m_MinDensity = 0.8f;
	float						m_MaxDensity = 1.2f;
	uint						m_NumMetaBalls = 20;
	uint						m_VoxelBrickRatio = 8;

protected:
	ComputePSO					m_CptUpdatePSO[kNumBufferType][kNumVolStructType];
	GraphicsPSO					m_GfxRenderPSO[kNumBufferType][kNumVolStructType];
	GraphicsPSO					m_GfxBrickMinMaxPSO;
	RootSignature				m_RootSignature;

	VolumeTexture				m_VolumeTexBuf[2];
	StructuredBuffer			m_StructuredVolBuf[2];
	TypedBuffer					m_TypedBuffer[2] = {DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT};

	StructuredBuffer			m_VertexBuffer;
	ByteAddressBuffer			m_IndexBuffer;

	PerFrameDataCB				m_PerFrameCBData;
	PerCallDataCB				m_PerCallCBData;

	// System will prepare new volume in bg so we need sync control
	uint8_t						m_OnStageIdx = 0;
	// Foreground and background sync signal
	std::atomic<ResourceState>	m_atomicState = kNormal;
	// Thread variable for background volume preparation thread
	std::thread					m_BgThread;
	// FenceValue for signal safe old buffer destroy
	uint64_t					m_FenceValue;
	// Default volume settings for preparing new volume
	BufferType					m_NewBufferType = m_BufferTypeInUse;
	VolStructType				m_NewVolstructType = m_VolStructTypeInUse;
	uint32_t					m_NewWidth = m_WidthInUse;
	uint32_t					m_NewHeight = m_HeightInUse;
	uint32_t					m_NewDepth = m_DepthInUse;
	// info. to control volume update, processed by cpu
	std::vector<Ball>			m_vecBalls;
	// Detect typeuav load support
	bool						m_TypeLoadSupported = false;

	double						m_AnimateTime = 0.0;
	bool						m_Animated = true;
};