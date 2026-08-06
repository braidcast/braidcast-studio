#include "uuid_util.hpp"

#include <util/platform.h>
#include <util/util.hpp>

namespace UuidUtil {

std::string New()
{
	BPtr<char> id = os_generate_uuid();
	return id ? std::string(id) : std::string();
}

} // namespace UuidUtil
