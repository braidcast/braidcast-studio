#include "StreamProfile.hpp"

std::string StreamProfile::PlatformName() const
{
	if (serviceId == "whip_custom") {
		return "WHIP";
	}
	if (serviceId == "rtmp_custom") {
		return "Custom";
	}
	const char *svc = settings ? obs_data_get_string(settings, "service") : "";
	if (svc && *svc) {
		/* rtmp_common service strings look like "YouTube - RTMPS"; take the
		 * platform portion before the first " - ". */
		std::string s = svc;
		size_t dash = s.find(" - ");
		return dash == std::string::npos ? s : s.substr(0, dash);
	}
	return "RTMP";
}

std::string StreamProfile::DisplayName() const
{
	std::string platform = PlatformName();
	if (label.empty()) {
		return platform;
	}
	return platform + " - " + label;
}

std::string StreamProfile::Key() const
{
	if (!settings) {
		return "";
	}
	const char *key = obs_data_get_string(settings, serviceId == "whip_custom" ? "bearer_token" : "key");
	return key ? key : "";
}

OBSDataAutoRelease StreamProfile::ToData() const
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "uuid", uuid.c_str());
	obs_data_set_string(data, "label", label.c_str());
	obs_data_set_string(data, "service_id", serviceId.c_str());
	obs_data_set_bool(data, "primary", isPrimary);
	obs_data_set_string(data, "account_id", accountId.c_str());
	if (settings) {
		obs_data_set_obj(data, "settings", settings);
	}
	return data;
}

StreamProfile StreamProfile::FromData(obs_data_t *data)
{
	StreamProfile p;
	p.uuid = obs_data_get_string(data, "uuid");
	p.label = obs_data_get_string(data, "label");
	const char *sid = obs_data_get_string(data, "service_id");
	if (sid && *sid) {
		p.serviceId = sid;
	}
	p.isPrimary = obs_data_get_bool(data, "primary");
	p.accountId = obs_data_get_string(data, "account_id");
	p.settings = obs_data_get_obj(data, "settings"); // null-safe; returns owning ref or null
	return p;
}
