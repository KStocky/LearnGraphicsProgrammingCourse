#pragma once
#ifdef __HLSL_VERSION
#define TRANSFER_STAGE 5
#else
inline constexpr Stage kStage = Stage::Flip;
#endif
