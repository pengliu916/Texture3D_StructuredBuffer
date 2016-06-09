#include "SparseVolume_SharedHeader.inl"
#if COMPUTE_SHADER
#	if TYPED_UAV
RWBuffer<float4> tex_uavDataVol : register(u0);
#	endif // TYPED_UAV
#	if STRUCT_UAV
RWStructuredBuffer<float4> tex_uavDataVol : register(u0);
#	endif // STRUCT_UAV
#	if TEX3D_UAV
RWTexture3D<float4> tex_uavDataVol : register(u0);
#	endif // TEX3D_UAV
#	if ENABLE_BRICKS
RWTexture3D<int> tex_uavFlagVol : register(u1);
#	endif // ENABLE_BRICKS
//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
float Ball( float3 f3Pos, float3 f3Center, float fRadiusSq )
{
	float3 f3d = f3Pos - f3Center;
	float fDistSq = dot( f3d, f3d );
	float fInvDistSq = 1.f / fDistSq;
	return fRadiusSq * fInvDistSq;
}

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads( THREAD_X, THREAD_Y, THREAD_Z )]
void cs_volumeupdate_main( uint3 u3DTid: SV_DispatchThreadID )
{
	// Current voxel pos in local space
	float3 currentPos = (u3DTid - f3VoxelReso*0.5f + 0.5f) * fVoxelSize;
	// Voxel content: x-density, yzw-color
	float4 f4Field = float4(0.f, 1.f, 1.f, 1.f);
	// Update voxel based on its position
	for (uint i = 0; i < uNumOfBalls; i++) {
		float fDensity = Ball( currentPos, f4Balls[i].xyz, f4Balls[i].w );
		f4Field.x += fDensity;
		f4Field.yzw += f4BallsCol[i].xyz * pow( fDensity, 3 ) * 1000.f;
	}
	// Make color vivid
	f4Field.yzw = normalize( f4Field.yzw );
	// Write back to voxel 
	uint IDX = u3DTid.x + u3DTid.y*f3VoxelReso.x + u3DTid.z*f3VoxelReso.x*f3VoxelReso.y;
#	if TEX3D_UAV
	f4Field.yzw = f4Field.zwy;
	tex_uavDataVol[u3DTid] = f4Field;
#	else
	tex_uavDataVol[IDX] = f4Field;
#	endif // TEX3D_UAV
#	if ENABLE_BRICKS
	// Update brick structure
	if (f4Field.x >= f2MinMaxDensity.x && field.x <= f2MinMaxDensity.y) tex_uavFlagVol[u3DTid / uVoxelBrickRatio] = 1;
#	endif // ENABLE_BRICKS
}
#endif // COMPUTE_SHADER

//#if PASSTHROUGH_VS
//void vs_passthrough_main()
//{}
//#endif // PASSTHROUGH_VS

#if CUBE_VS
void vs_cube_main( inout float4 f4Pos : POSITION, out float4 f4ProjPos : SV_POSITION )
{
	f4Pos.xyz *= (f3VoxelReso * fVoxelSize);
	f4ProjPos = mul( mWorldViewProj, f4Pos );
}
#endif // CUBE_VS

#if RAYCAST_PS
#	if TYPED_UAV
Buffer<float4> tex_srvDataVol : register(t0);
#	endif // TYPED_UAV
#	if STRUCT_UAV
StructuredBuffer<float4> tex_srvDataVol : register(t0);
#	endif // STRUCT_UAV
#	if TEX3D_UAV
Texture3D<float4> tex_srvDataVol : register(t0);
#	endif // TEX3D_UAV
#if ENABLE_BRICKS
Texture3D<int> tex_srvFlagVol : register(t1);
#endif // ENABLE_BRICKS
SamplerState sam_Linear : register(s0);
struct Ray
{
	float4 f4o;
	float4 f4d;
};
//--------------------------------------------------------------------------------------
// Utility Funcs
//--------------------------------------------------------------------------------------
bool IntersectBox( Ray r, float3 boxmin, float3 boxmax, out float tnear, out float tfar )
{
	// compute intersection of ray with all six bbox planes
	float3 invR = 1.0 / r.f4d.xyz;
	float3 tbot = invR * (boxmin.xyz - r.f4o.xyz);
	float3 ttop = invR * (boxmax.xyz - r.f4o.xyz);

	// re-order intersections to find smallest and largest on each axis
	float3 tmin = min( ttop, tbot );
	float3 tmax = max( ttop, tbot );

	// find the largest tmin and the smallest tmax
	float2 t0 = max( tmin.xx, tmin.yz );
	tnear = max( t0.x, t0.y );
	t0 = min( tmax.xx, tmax.yz );
	tfar = min( t0.x, t0.y );

	return tnear <= tfar;
}

float3 world2uv( float3 f3P )
{
	float3 f3uv = f3P * f3InvVolSize + 0.5f;
	f3uv.y = 1 - f3uv.y;
	return f3uv;
}
//
//void isoSurfaceShading( Ray eyeray, float2 f2NearFar, float isoValue,
//	inout float4 f4OutColor, inout float fOutDepth )
//{
//	float3 f3Pnear = eyeray.o.xyz + eyeray.d.xyz * f2NearFar.x;
//	float3 f3Pfar = eyeray.o.xyz + eyeray.d.xyz * f2NearFar.y;
//
//	float3 f3P = f3Pnear;
//	float t = f2NearFar.x;
//	float fStep = 0.99 * voxelSize;
//	float3 P_pre = f3Pnear;
//	float3 PsmallStep = eyeray.d.xyz * fStep;
//
//	float4 surfacePos;
//
//	float field_pre;
//	float field_now = g_srvDataVolume.SampleLevel( sam_Linear, world2uv( f3P ), 0 ).x;
//
//	while (t <= f2NearFar.y) {
//		float3 f3TexCoord = world2uv( f3P );
//		float4 Field = g_srvDataVolume.SampleLevel( sam_Linear, f3TexCoord, 0 );
//		//float4 Field = g_srvDataVolume.Load( int4(f3TexCoord*voxelInfo.xyz,0));
//
//		float density = Field.x;
//		float4 color = float4(Field.yzw, 0);
//
//		field_pre = field_now;
//		field_now = density;
//
//		if (field_now >= isoValue && field_pre < isoValue)
//		{
//			// For computing the depth
//			surfacePos = float4(P_pre + (f3P - P_pre) * (isoValue - field_pre) / (field_now - field_pre), 1.f);
//
//			// For computing the normal
//
//			float3 tCoord = world2uv( surfacePos.xyz );
//			float depth_dx = g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (1, 0, 0) / voxelInfo.xyz, 0 ).x -
//				g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (-1, 0, 0) / voxelInfo.xyz, 0 ).x;
//			float depth_dy = g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (0, -1, 0) / voxelInfo.xyz, 0 ).x -
//				g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (0, 1, 0) / voxelInfo.xyz, 0 ).x;
//			float depth_dz = g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (0, 0, 1) / voxelInfo.xyz, 0 ).x -
//				g_srvDataVolume.SampleLevel( sam_Linear, tCoord + float3 (0, 0, -1) / voxelInfo.xyz, 0 ).x;
//
//			float3 normal = -normalize( float3 (depth_dx, depth_dy, depth_dz) );
//
//
//			// shading part
//			float3 ambientLight = aLight_col * color;
//
//			float3 directionalLight = dLight_col * color * clamp( dot( normal, dLight_dir ), 0, 1 );
//
//			float3 vLight = cb_f4ViewPos.xyz - surfacePos.xyz;
//			float3 halfVect = normalize( vLight - eyeray.d.xyz );
//			float dist = length( vLight ); vLight /= dist;
//			float angleAttn = clamp( dot( normal, vLight ), 0, 1 );
//			float distAttn = 1.0f / (dist * dist);
//			float specularAttn = pow( clamp( dot( normal, halfVect ), 0, 1 ), 128 );
//
//			float3 pointLight = pLight_col * color * angleAttn + color * specularAttn;
//
//			f4OutColor = float4(ambientLight + directionalLight + pointLight, 1);
//			surfacePos = mul( surfacePos, cb_mInvView );
//			fOutDepth = surfacePos.z / 10.f;
//			return;
//			//return float4(normal*0.5+0.5,0);
//		}
//
//		P_pre = f3P;
//		f3P += PsmallStep;
//		t += fStep;
//	}
//	return;
//}

float transferFunction( float fDensity )
{
	float fOpacity = (fDensity - f2MinMaxDensity.x) / (f2MinMaxDensity.y - f2MinMaxDensity.x);
	float fp2 = fOpacity*fOpacity + 0.02f;
	float fp4 = fp2*fp2;
	return fp4*0.3f + fp2*0.1f + fOpacity*0.15f;
}

void accumulatedShading( Ray eyeray, float2 f2NearFar, float2 f2MinMaxDen,
#if DRAW_BRICKGRID
	inout float4 f4OutColor, inout float fOutDepth )
{
	bool bFirstEnter = true;
#else
	inout float4 f4OutColor)
{
#endif // DRAW_BRICKGRID
	float3 f3Pnear = eyeray.f4o.xyz + eyeray.f4d.xyz * f2NearFar.x;
	float3 f3Pfar = eyeray.f4o.xyz + eyeray.f4d.xyz * f2NearFar.y;

	float3 f3P = f3Pnear;
	float t = f2NearFar.x;
	float fStep = 0.8 * fVoxelSize;
	float3 f3PsmallStep = eyeray.f4d.xyz * fStep;

	uint3 idx = world2uv( f3P )*f3VoxelReso + 0.5f;
	uint IDX = idx.x + idx.y*f3VoxelReso.x + idx.z*f3VoxelReso.x*f3VoxelReso.y;
#if TEX3D_UAV
	float field_now = tex_srvDataVol[idx].x;
#else
	float field_now = tex_srvDataVol[IDX].x;
#endif // TEX3D_UAV
	float4 f4AccuData = 0;
	while (t <= f2NearFar.y) {
	float4 f4CurData = float4(0.001, 0.001, 0.001, 0.005f);
#if DRAW_BRICKGRID
		if (bFirstEnter) {
			float4 fPos = mul( float4(f3P, 1.f), mView );
			fOutDepth = fPos.z / 10.f;
			bFirstEnter = false;
		}
#endif // DRAW_BRICKGRID
		float3 f3TexCoord = world2uv( f3P );
		//float4 f4Field = tex_srvDataVol.SampleLevel( sam_Linear, f3TexCoord, 0 );
		uint3 idx = f3TexCoord*f3VoxelReso + 0.5f;
		IDX = idx.x + idx.y*f3VoxelReso.x + idx.z*f3VoxelReso.x*f3VoxelReso.y;
#if TEX3D_UAV
		float4 f4Field = tex_srvDataVol[idx];
#else
		float4 f4Field = tex_srvDataVol[IDX];
#endif // TEX3D_UAV

		if (f4Field.x >= f2MinMaxDen.x && f4Field.x <= f2MinMaxDen.y)
		{
			f4CurData = float4(f4Field.yzw, transferFunction( f4Field.x ));
			f4CurData.a *= 0.25f;

			f4CurData.rgb *= f4CurData.a;
		}
		f4AccuData = (1.0f - f4AccuData.a)*f4CurData + f4AccuData;
		//if (f4AccuData.a >= 0.95f) break;
		f3P += f3PsmallStep;
		t += fStep;
	}
	f4OutColor = f4AccuData*f4AccuData.a;
	return;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 ps_raycast_main( float4 f4Pos : POSITION, float4 f4ProjPos : SV_POSITION ) : SV_Target
{
	Ray eyeray;
	//world space
	eyeray.f4o = f4ViewPos;
	eyeray.f4d = f4Pos - eyeray.f4o;
	eyeray.f4d = normalize( eyeray.f4d );
	eyeray.f4d.x = (eyeray.f4d.x == 0.f) ? 1e-15 : eyeray.f4d.x;
	eyeray.f4d.y = (eyeray.f4d.y == 0.f) ? 1e-15 : eyeray.f4d.y;
	eyeray.f4d.z = (eyeray.f4d.z == 0.f) ? 1e-15 : eyeray.f4d.z;
	
	// calculate ray intersection with bounding box
	float fTnear, fTfar;
	bool bHit = IntersectBox( eyeray, f3BoxMin, f3BoxMax , fTnear, fTfar );
	if (!bHit) discard;
	if (fTnear <= 0) fTnear = 0;
	float4 f4Col = float4(1.f, 1.f, 1.f, 0.f) * 0.01f;
	float fDepth = 1000.f;
	
	//isoSurfaceShading(eyeray, float2(tnear,tfar),invVolSize.w, col,depth);
	accumulatedShading( eyeray, float2(fTnear,fTfar),f2MinMaxDensity, f4Col);
	
	return f4Col;
}
#endif // RAYCAST_PS