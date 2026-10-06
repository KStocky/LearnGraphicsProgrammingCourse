#pragma once
#ifdef __HLSL_VERSION
#define TRANSFER_STAGE 0
#else
inline constexpr Stage kStage = Stage::Identity;
#endif
