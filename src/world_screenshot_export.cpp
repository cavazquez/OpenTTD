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
#include "transparency.h"
#include "viewport_func.h"
#include "video/video_driver.hpp"
#include "window_func.h"

#include <chrono>
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

bool EnvEnabled(const char *name)
{
	const char *raw = std::getenv(name);
	if (raw == nullptr || raw[0] == '\0') return false;
	const std::string_view value(raw);
	return value != "0" && value != "false" && value != "no" && value != "off";
}

int WorldScreenshotMinCall()
{
	const char *raw = std::getenv("OPENTTDRS_WORLD_SCREENSHOT_MIN_CALL");
	if (raw == nullptr || raw[0] == '\0') return 2;
	const int requested = std::atoi(raw);
	return requested > 0 ? requested : 2;
}

/**
 * `SC_DEFAULTZOOM` reusa la esquina virtual del viewport principal, pero le
 * puede pedir al raster un tamaño distinto al de la ventana headless. Si no
 * corregimos esa esquina, `ScrollMainWindowToTile` centra la tesela en el
 * viewport original y la captura recortada queda desplazada. Mantener el
 * centro virtual evita que el oráculo compare regiones distintas al cambiar
 * la resolución.
 */
void CenterScreenshotViewportOnMainWindow(Window &window, uint32_t width, uint32_t height)
{
	ViewportData &viewport = *window.viewport;
	const uint32_t zoom_factor = 1U << to_underlying(ZoomLevel::Viewport);
	const int requested_virtual_width = static_cast<int>(width * zoom_factor);
	const int requested_virtual_height = static_cast<int>(height * zoom_factor);
	const int delta_x = (viewport.virtual_width - requested_virtual_width) / 2;
	const int delta_y = (viewport.virtual_height - requested_virtual_height) / 2;
	/* `UpdateViewportPosition` deriva virtual_left/top de scrollpos. Ajustar
	 * solamente los campos virtuales duraría hasta el siguiente tick; mover
	 * ambas posiciones de scroll conserva el recorte hasta que MakeScreenshot
	 * consume el viewport en la tarea encolada. */
	viewport.scrollpos_x += delta_x;
	viewport.dest_scrollpos_x += delta_x;
	viewport.scrollpos_y += delta_y;
	viewport.dest_scrollpos_y += delta_y;
}

void LogScreenshotViewport(const Window &window, uint32_t width, uint32_t height)
{
	if (!EnvEnabled("OPENTTDRS_WORLD_SCREENSHOT_DEBUG")) return;
	const ViewportData &viewport = *window.viewport;
	std::fprintf(stderr,
		"openttdrs world-screenshot: scroll=(%d,%d) dest=(%d,%d) virtual=(%d,%d %dx%d) capture=%ux%u\n",
		viewport.scrollpos_x, viewport.scrollpos_y,
		viewport.dest_scrollpos_x, viewport.dest_scrollpos_y,
		viewport.virtual_left, viewport.virtual_top,
		viewport.virtual_width, viewport.virtual_height, width, height);
}

/**
 * Normaliza las capas que son inherentemente temporales o configurables para
 * que una captura de paridad mida terreno e infraestructura, no nombres de
 * pueblos/estaciones ni la carrera entre dos loops de simulación.
 */
void PrepareCleanWorldScreenshot()
{
	_pause_mode.Set(PauseMode::Normal);
	ClrBit(_display_opt, DO_SHOW_TOWN_NAMES);
	ClrBit(_display_opt, DO_SHOW_STATION_NAMES);
	ClrBit(_display_opt, DO_SHOW_WAYPOINT_NAMES);
	ClrBit(_display_opt, DO_SHOW_SIGNS);
	ClrBit(_display_opt, DO_SHOW_COMPETITOR_SIGNS);
	ClrBit(_display_opt, DO_FULL_ANIMATION);
}

} // namespace

bool OpenttdrsMaybeCaptureWorldScreenshot()
{
	const char *output = std::getenv("OPENTTDRS_WORLD_SCREENSHOT_OUT");
	if (output == nullptr || output[0] == '\0') return true;

	/* Dedicated + -g primero carga una partida temporal. Igual que los
	 * exportadores raw/semantic/draw, ignorar ese primer AfterLoadGame evita
	 * que el PNG pertenezca al mapa de arranque en lugar del .sav solicitado. */
	static int call_count = 0;
	call_count++;
	if (call_count < WorldScreenshotMinCall()) return true;

	if (EnvEnabled("OPENTTDRS_WORLD_SCREENSHOT_CLEAN")) {
		PrepareCleanWorldScreenshot();
	}

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
			const bool moved = ScrollMainWindowToTile(*center, true);
			if (EnvEnabled("OPENTTDRS_WORLD_SCREENSHOT_DEBUG")) {
				std::fprintf(stderr, "openttdrs world-screenshot: focus=(%u,%u) moved=%d\n",
					TileX(*center), TileY(*center), moved);
			}
			/* El scroll instantáneo actualiza scrollpos, pero la captura de
			 * viewport consume virtual_left/virtual_top. En una ejecución
			 * headless no esperamos el siguiente DrawOverlappedWindow; forzamos
			 * la misma actualización que haría ese frame antes de capturar. */
			if (Window *main_window = GetMainWindow(); main_window != nullptr) {
				UpdateViewportPosition(main_window, 0);
				CenterScreenshotViewportOnMainWindow(*main_window, width, height);
				UpdateViewportPosition(main_window, 0);
				LogScreenshotViewport(*main_window, width, height);
			}
		}

		/* `-x` deliberately starts from a blank config while exporting a reference.
		 * The normal setting therefore has an empty screenshot format, which makes
		 * the screenshot provider lookup fail. PNG is built into our reference
		 * configuration and gives the comparison script a deterministic artifact. */
		_screenshot_format_name = "png";
		/* MakeScreenshot only reports that its work was queued. Give each
		 * request a fresh internal name so that a failed queued raster cannot
		 * make us copy an older successful PNG from a previous invocation. */
		const std::string screenshot_name = "openttdrs-world-reference-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		if (!MakeScreenshot(SC_DEFAULTZOOM, screenshot_name, width, height)) {
			std::fprintf(stderr, "openttdrs world-screenshot: no se pudo encolar la captura\n");
			_exit_game = true;
			return;
		}

		/* MakeScreenshot encola primero el raster. Esta segunda tarea queda
		 * detrás de él, por lo que `_full_screenshot_path` ya corresponde a
		 * esta captura. Si el raster falló no existe un archivo con el nombre
		 * nuevo: abortar es preferible a copiar una referencia obsoleta. */
		VideoDriver::GetInstance()->QueueOnMainThread([target] {
			std::error_code error;
			const std::filesystem::path source(_full_screenshot_path);
			if (!std::filesystem::is_regular_file(source, error) || error ||
					std::filesystem::file_size(source, error) == 0 || error) {
				std::fprintf(stderr, "openttdrs world-screenshot: el raster no produjo PNG nuevo\n");
				_exit_game = true;
				return;
			}
			const std::filesystem::path destination(target);
			if (!destination.parent_path().empty()) {
				std::filesystem::create_directories(destination.parent_path(), error);
			}
			if (!error) {
				std::filesystem::copy_file(
					source,
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

bool OpenttdrsWorldScreenshotHideVehicles()
{
	return EnvEnabled("OPENTTDRS_WORLD_SCREENSHOT_CLEAN");
}
