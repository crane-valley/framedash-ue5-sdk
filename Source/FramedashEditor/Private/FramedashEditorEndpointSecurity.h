// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include <string>
#include <string_view>

namespace FramedashEditor
{
	std::string ExtractUrlHost(std::string_view Url);
	bool IsEndpointSecure(std::string_view Url);
	bool IsCrossOriginRedirect(std::string_view ConfiguredUrl, std::string_view EffectiveUrl);
}
