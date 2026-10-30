#include "std_include.hpp"
#include "weather.hpp"

#include <sstream>

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

namespace comp::game::weather
{
	namespace
	{
		using shared::common::WEATHER_CLASS_COUNT;
		using shared::common::WEATHER_CLASS_NAMES;
		using LT = shared::common::LOG_TYPE;

		enum Class { CLEAR, HAZE, CLOUDY, OVERCAST, FOG, DUST, RAIN, STORM, SNOW };

		constexpr uint8_t FLAG_CLOUDY = 0x02;
		constexpr uint8_t FLAG_RAINY  = 0x04;
		constexpr uint8_t FLAG_SNOW   = 0x08;

		// Frames a new sky must read back unchanged before it is reported, and
		// frames the first report holds the blend at 1 (a snap).
		constexpr int SETTLE_FRAMES = 30;
		constexpr int SNAP_FRAMES = 30;

		void log(const std::string& msg, LT type = LT::LOG_TYPE_DEFAULT)
		{
			shared::common::log("Weather", msg, type, false);
		}

		std::string class_id(int c)
		{
			std::string id = "fnv.";
			for (const char* p = WEATHER_CLASS_NAMES[c]; *p; ++p)
				id += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
			return id;
		}

		std::string trim(const std::string& s)
		{
			const size_t a = s.find_first_not_of(" \t\r\n");
			if (a == std::string::npos) return {};
			return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
		}

		bool iequals(const std::string& a, const std::string& b)
		{
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}

		// The fog distance the class is judged on: the day's far fog, else the
		// night's (some records leave the day pair at 0). 0 = no fog given.
		float fog_far(const WeatherRecord& w)
		{
			if (w.fog_day_far > 0.0f) return w.fog_day_far;
			return w.fog_night_far > 0.0f ? w.fog_night_far : 0.0f;
		}

		int classify(const WeatherRecord& w)
		{
			const auto& c = shared::common::config::get().weather;
			if (w.classification & FLAG_RAINY)
				return static_cast<int>(w.thunder_freq) <= c.storm_thunder_max ? STORM : RAIN;
			if (w.classification & FLAG_SNOW) return SNOW;

			const float fog = fog_far(w);
			const bool windy = w.wind >= c.dust_wind_min;
			if (fog > 0.0f && fog <= c.fog_far_max) return windy ? DUST : FOG;
			if (fog > 0.0f && fog <= c.haze_fog_far_max)
			{
				if (windy) return DUST;
				return (w.classification & FLAG_CLOUDY) ? OVERCAST : HAZE;
			}
			return (w.classification & FLAG_CLOUDY) ? CLOUDY : CLEAR;
		}

		std::string ids(const WeatherRecord& w, int cls)
		{
			return std::format("{:08X}, {}", w.form_id, class_id(cls));
		}

		std::string describe(const WeatherRecord& w, int cls)
		{
			const std::string& preset = shared::common::config::get().weather.preset[cls];
			return std::format("{:08X} {} (flags 0x{:02X} wind {} thunder {} fog {:.0f}) -> preset {}",
				w.form_id, WEATHER_CLASS_NAMES[cls], w.classification, w.wind, w.thunder_freq, fog_far(w),
				preset.empty() ? std::string("(unbound)") : preset);
		}

		// One rtx.weather.* option, sent only when its value changes.
		struct pushed
		{
			const char* key;
			std::string last;
			bool sent = false;

			void push(const std::string& value)
			{
				if (sent && value == last) return;
				auto& api = shared::common::remix_api::get();
				if (!api.m_bridge.SetConfigVariable) return;
				api.m_bridge.SetConfigVariable(key, value.c_str());
				last = value;
				sent = true;
			}
		};

		pushed s_current  { "rtx.weather.gameWeather" };
		pushed s_outgoing { "rtx.weather.gameWeatherOutgoing" };
		pushed s_blend    { "rtx.weather.gameWeatherBlend" };

		const void* s_sky = nullptr;
		int s_settle = 0;
		int s_snap = 0;
		bool s_reported_none = false;

		// What the last log line described, to log changes only.
		uint32_t s_logged_current = 0;
		uint32_t s_logged_outgoing = 0;
		bool s_logged_none = false;
		uint32_t s_frame = 0;
		std::string s_current_name;

		void report_none(const char* why)
		{
			s_current.push("");
			s_outgoing.push("");
			s_blend.push("-1");
			if (!s_logged_none)
			{
				s_logged_none = true;
				s_logged_current = s_logged_outgoing = 0;
				s_current_name = "none";
				log(std::format("no weather ({}): NUMOS shows its chosen preset or its settings", why));
			}
		}
	}

	void bind_presets()
	{
		namespace fs = std::filesystem;
		const auto& c = shared::common::config::get().weather;
		if (!c.enabled || !c.bind_presets) return;

		fs::path dir(c.preset_directory);
		if (dir.is_relative()) dir = fs::path(shared::globals::root_path) / dir;
		std::error_code ec;
		if (!fs::is_directory(dir, ec))
		{
			log(std::format("preset folder {} not found: the runtime writes NUMOS's presets there on its first sky "
				"frame, and the classes are bound at the next start", dir.string()), LT::LOG_TYPE_WARN);
			return;
		}

		std::string class_ids[WEATHER_CLASS_COUNT];
		bool found[WEATHER_CLASS_COUNT] = {};
		for (int i = 0; i < WEATHER_CLASS_COUNT; i++) class_ids[i] = class_id(i);

		for (const auto& entry : fs::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".conf") continue;
			const std::string stem = entry.path().stem().string();

			std::string text;
			{
				std::ifstream f(entry.path(), std::ios::binary);
				if (!f) continue;
				std::ostringstream ss;
				ss << f.rdbuf();
				text = ss.str();
			}

			// The weather.bindings line: [begin, end) without its line ending.
			size_t begin = std::string::npos, end = std::string::npos;
			for (size_t pos = 0; pos < text.size();)
			{
				size_t nl = text.find('\n', pos);
				if (nl == std::string::npos) nl = text.size();
				size_t line_end = nl;
				if (line_end > pos && text[line_end - 1] == '\r') line_end--;
				std::string line = text.substr(pos, line_end - pos);
				const size_t hash = line.find('#');
				if (hash != std::string::npos) line.resize(hash);
				const size_t eq = line.find('=');
				if (eq != std::string::npos && trim(line.substr(0, eq)) == "weather.bindings")
				{
					begin = pos;
					end = line_end;
					break;
				}
				pos = nl + 1;
			}

			std::vector<std::string> old_ids, new_ids;
			if (begin != std::string::npos)
			{
				std::string value = text.substr(begin, end - begin);
				const size_t hash = value.find('#');
				if (hash != std::string::npos) value.resize(hash);
				value = value.substr(value.find('=') + 1);
				std::istringstream list(value);
				std::string id;
				while (std::getline(list, id, ','))
				{
					id = trim(id);
					if (!id.empty()) old_ids.push_back(id);
				}
			}

			// The user's own ids stay, in their order; the class ids follow the table.
			for (const std::string& id : old_ids)
			{
				bool ours = false;
				for (const std::string& cid : class_ids) ours |= iequals(id, cid);
				if (!ours) new_ids.push_back(id);
			}
			for (int i = 0; i < WEATHER_CLASS_COUNT; i++)
			{
				if (c.preset[i].empty() || !iequals(c.preset[i], stem)) continue;
				found[i] = true;
				new_ids.push_back(class_ids[i]);
			}

			bool same = old_ids.size() == new_ids.size();
			for (size_t i = 0; same && i < old_ids.size(); i++) same = old_ids[i] == new_ids[i];
			if (same) continue;

			std::string line = "weather.bindings = ";
			for (size_t i = 0; i < new_ids.size(); i++) line += (i ? ", " : "") + new_ids[i];
			if (begin != std::string::npos)
				text.replace(begin, end - begin, line);
			else
				text.insert(0, line + (text.find("\r\n") != std::string::npos ? "\r\n" : "\n"));

			std::ofstream f(entry.path(), std::ios::binary | std::ios::trunc);
			if (!f || !(f << text))
			{
				log(std::format("cannot write {}", entry.path().string()), LT::LOG_TYPE_WARN);
				continue;
			}
			std::string before;
			for (size_t i = 0; i < old_ids.size(); i++) before += (i ? ", " : "") + old_ids[i];
			log(std::format("{}.conf: weather.bindings '{}' -> '{}'", stem, before, line.substr(19)));
		}

		for (int i = 0; i < WEATHER_CLASS_COUNT; i++)
		{
			if (c.preset[i].empty())
				log(std::format("class {} unbound ([Weather] {} is empty)", WEATHER_CLASS_NAMES[i], WEATHER_CLASS_NAMES[i]));
			else if (!found[i])
				log(std::format("class {}: no preset {}.conf in {}, the class stays unbound",
					WEATHER_CLASS_NAMES[i], c.preset[i], dir.string()), LT::LOG_TYPE_WARN);
		}
	}

	void update()
	{
		if (!shared::common::remix_api::get().is_initialized()) return;
		s_frame++;

		if (!shared::common::config::get().weather.enabled)
		{
			// Leave the runtime's weather as if the wrapper never set it.
			if (!s_reported_none && (s_current.sent || s_outgoing.sent || s_blend.sent))
			{
				s_current.push("");
				s_outgoing.push("");
				s_blend.push("-1");
				log("Enabled=0: no game weather reported");
			}
			s_reported_none = true;
			s_sky = nullptr;
			return;
		}
		s_reported_none = false;

		const void* sky = try_get_sky();
		if (!sky)
		{
			s_sky = nullptr;
			report_none("no sky");
			return;
		}
		if (sky != s_sky)
		{
			s_sky = sky;
			s_settle = 0;
			return;
		}
		if (s_settle < SETTLE_FRAMES)
		{
			if (++s_settle == SETTLE_FRAMES) s_snap = SNAP_FRAMES;
			return;
		}

		WeatherState st{};
		if (!try_get_weather_state(st))
		{
			report_none("the sky's weather did not read back");
			return;
		}
		if (!st.has_current)
		{
			s_snap = SNAP_FRAMES;
			report_none("the sky has no current weather");
			return;
		}

		const int cur_class = classify(st.current);
		const std::string cur_ids = ids(st.current, cur_class);
		int out_class = -1;
		if (st.has_outgoing)
		{
			out_class = classify(st.outgoing);
			s_snap = 0;
			s_current.push(cur_ids);
			s_outgoing.push(ids(st.outgoing, out_class));
			s_blend.push(std::format("{:.3f}", st.percent));
		}
		else if (s_snap > 0)
		{
			s_snap--;
			s_current.push(cur_ids);
			s_outgoing.push(cur_ids);
			s_blend.push("1");
		}
		else
		{
			s_current.push(cur_ids);
			s_outgoing.push("");
			s_blend.push("-1");
		}

		const uint32_t out_id = st.has_outgoing ? st.outgoing.form_id : 0;
		if (s_logged_none || st.current.form_id != s_logged_current || out_id != s_logged_outgoing)
		{
			if (st.has_outgoing)
				log(std::format("transition {:.0f}%: {}  <-  from {}", st.percent * 100.0f,
					describe(st.current, cur_class), describe(st.outgoing, out_class)));
			else if (s_logged_outgoing && st.current.form_id == s_logged_current)
				log(std::format("transition complete: {}", describe(st.current, cur_class)));
			else
				log(std::format("{}: {}", s_snap > 0 ? "weather (snap)" : "weather (runtime fade)",
					describe(st.current, cur_class)));
			s_logged_none = false;
			s_logged_current = st.current.form_id;
			s_current_name = std::format("{:08X} {}", st.current.form_id, WEATHER_CLASS_NAMES[cur_class]);
			s_logged_outgoing = out_id;
		}
		else if (st.has_outgoing && (s_frame % 600) == 0)
		{
			log(std::format("transition {:.0f}% to {:08X}", st.percent * 100.0f, st.current.form_id));
		}
	}

	const std::string& current()
	{
		return s_current_name;
	}
}
