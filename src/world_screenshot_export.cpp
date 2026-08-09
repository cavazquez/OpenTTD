/*
 * Reproducible raster reference for openttdrs world-render parity.
 *
 * The regular screenshot implementation owns image encoding and the target
 * screenshot directory. This helper only centers the main viewport, queues a
 * normal-zoom viewport render, copies the resulting PNG to an explicit path,
 * and exits once the queued render completed.
 */

#include "world_screenshot_export.h"

#include "map_func.h"
#include "openttd.h"
#include "screenshot.h"
#include "viewport_func.h"
#include "video/video_driver.hpp"
#include "window_func.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool ParseUint(std::string_view text, uint32_t &value)
{
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc{} && end == text.data() + text.size();
}

bool ParseCenter(const char *raw, uint32_t &x, uint32_t &y)
{
	if (raw == nullptr || raw[0] == '\0') return false;
	const std::string_view text(raw);
	const size_t separator = text.find(',');
	if (separator == std::string_view::npos) return false;
	return ParseUint(text.substr(0, separator), x) && ParseUint(text.substr(separator + 1), y);
}

bool ParseResolution(const char *raw, uint32_t &width, uint32_t &height)
{
	if (raw == nullptr || raw[0] == '\0') return true;
	const std::string_view text(raw);
	const size_t separator = text.find('x');
	if (separator == std::string_view::npos ||
			!ParseUint(text.substr(0, separator), width) ||
			!ParseUint(text.substr(separator + 1), height)) {
		return false;
	}
	return width > 0 && height > 0;
}

} // namespace

bool OpenttdrsMaybeCaptureWorldScreenshot()
{
	const char *output = std::getenv("OPENTTDRS_WORLD_SCREENSHOT_OUT");
	if (output == nullptr || output[0] == '\0') return true;

	uint32_t width = 1280;
	uint32_t height = 720;
	if (!ParseResolution(std::getenv("OPENTTDRS_WORLD_SCREENSHOT_RES"), width, height)) {
		std::fprintf(stderr, "openttdrs world-screenshot: resolución inválida (usar anchoxalto)\n");
		return false;
	}

	std::optional<TileIndex> center;
	if (const char *raw_center = std::getenv("OPENTTDRS_WORLD_SCREENSHOT_CENTER"); raw_center != nullptr) {
		uint32_t x = 0;
		uint32_t y = 0;
		if (!ParseCenter(raw_center, x, y) || x >= Map::SizeX() || y >= Map::SizeY()) {
			std::fprintf(stderr, "openttdrs world-screenshot: centro inválido %s (usar x,y dentro del mapa)\n", raw_center);
			return false;
		}
		center = TileXY(x, y);
	}

	const std::string target(output);
	/* En el driver dedicado, AfterLoadGame encola esto durante Tick N y el
	 * primer callback corre justo antes de Tick N+1, cuando OpenTTD todavía
	 * puede restaurar la cámara guardada. Diferimos un callback adicional:
	 * entonces el centrado se hace antes de Tick N+2, ya estable. */
	VideoDriver::GetInstance()->QueueOnMainThread([center, width, height, target] {
		VideoDriver::GetInstance()->QueueOnMainThread([center, width, height, target] {
		if (center.has_value()) {
			ScrollMainWindowToTile(*center, true);
			/* El scroll instantáneo actualiza scrollpos, pero la captura de
			 * viewport consume virtual_left/virtual_top. En una ejecución
			 * headless no esperamos el siguiente DrawOverlappedWindow; forzamos
			 * la misma actualización que haría ese frame antes de capturar. */
			if (Window *main_window = GetMainWindow(); main_window != nullptr) {
				UpdateViewportPosition(main_window, 0);
			}
		}

		/* `-x` deliberately starts from a blank config while exporting a reference.
		 * The normal setting therefore has an empty screenshot format, which makes
		 * the screenshot provider lookup fail. PNG is built into our reference
		 * configuration and gives the comparison script a deterministic artifact. */
		_screenshot_format_name = "png";
		if (!MakeScreenshot(SC_DEFAULTZOOM, "openttdrs-world-reference", width, height)) {
			std::fprintf(stderr, "openttdrs world-screenshot: no se pudo encolar la captura\n");
			_exit_game = true;
			return;
		}

		/* MakeScreenshot encola primero el raster. Esta segunda tarea queda
		 * detrás de él, por lo que `_full_screenshot_path` ya es el PNG final. */
		VideoDriver::GetInstance()->QueueOnMainThread([target] {
			std::error_code error;
			const std::filesystem::path destination(target);
			if (!destination.parent_path().empty()) {
				std::filesystem::create_directories(destination.parent_path(), error);
			}
			if (!error) {
				std::filesystem::copy_file(
					_full_screenshot_path,
					destination,
					std::filesystem::copy_options::overwrite_existing,
					error
				);
			}
			if (error) {
				std::fprintf(stderr, "openttdrs world-screenshot: no se pudo copiar a %s: %s\n", target.c_str(), error.message().c_str());
			} else {
				std::fprintf(stderr, "openttdrs world-screenshot: escrito %s\n", target.c_str());
			}
			_exit_game = true;
		});
		});
	});
	return true;
}
