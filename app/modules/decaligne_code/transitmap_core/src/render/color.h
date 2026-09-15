#pragma once

namespace Color {

	// background color
	inline float background[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	// Line obstalces and region fill and new route
	inline constexpr float obstacleRoute[3] = { 0.60f, 0.75f, 0.90f };// light blue
	inline constexpr float newRoute[3] = { 0.745f, 0.61f, 0.992f };// purple
	inline constexpr float regionFill[3] = { 0.72f, 0.90f, 0.72f };// light green

	// fixed points
	inline constexpr float fixedPoint[3] = { 0.90f, 0.20f, 0.20f };// red
	// psudo stations
	inline constexpr float pseudoStation[3] = { 0.30f, 0.42f, 0.75f };// dark blue
	//shape points
	inline constexpr float shapePoint[3] = { 0.20f, 0.20f, 0.20f }; // grey
	// stations
	inline constexpr float black[3] = { 0.0f, 0.0f, 0.0f };//black
	inline constexpr float white[3] = { 1.0f, 1.0f, 1.0f };//white

	// ImGui editing draft colors
	inline constexpr float editAlpha = 0.86f;// alpha for editing
	inline constexpr float noLoopRoute[4] = { 0.60f, 0.75f, 0.90f, 0.55f };// light blue
	inline constexpr float noLoopPoint[4] = { 0.10f, 0.22f, 0.55f, 0.60f };// dark blue
	inline constexpr float loopPoint[4] = { 0.12f, 0.45f, 0.20f, 0.60f };// dark green
	inline constexpr float draftPoint[4] = { 0.55f, 0.55f, 0.55f, 0.95f };//grey

	// displacement line color
	inline constexpr float displacementLine[4] = { 0.0f, 0.0f, 0.0f, 0.85f };// black with alpha

}
