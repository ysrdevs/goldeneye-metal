/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Rewired onto the current Runtime/RuntimeConfig API and
 *                     made fully headless + dependency-free (BMP/RAW dump),
 *                     for use as a backend reference/diff oracle.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_dump.h>
#include <rex/graphics/trace_player.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/xtypes.h>
#include <rex/ui/presenter.h>

namespace rex::graphics {

namespace {

// Write a 24-bit, bottom-up BMP from RGBX (R8 G8 B8 X8) pixel data. BMP is the
// simplest broadly-viewable format with zero dependencies; macOS Preview/sips
// open it directly. We also dump raw RGBA alongside for byte-exact diffing.
bool WriteBmp(const std::filesystem::path& path, const ui::RawImage& image) {
  const uint32_t w = image.width;
  const uint32_t h = image.height;
  if (!w || !h || image.data.empty()) {
    return false;
  }
  const uint32_t row_bytes = w * 3;
  const uint32_t padded_row = (row_bytes + 3u) & ~3u;
  const uint32_t pixel_array = padded_row * h;
  const uint32_t file_size = 54 + pixel_array;

  std::vector<uint8_t> buf(file_size, 0);
  auto put16 = [&](size_t off, uint16_t v) {
    buf[off] = uint8_t(v & 0xff);
    buf[off + 1] = uint8_t((v >> 8) & 0xff);
  };
  auto put32 = [&](size_t off, uint32_t v) {
    buf[off] = uint8_t(v & 0xff);
    buf[off + 1] = uint8_t((v >> 8) & 0xff);
    buf[off + 2] = uint8_t((v >> 16) & 0xff);
    buf[off + 3] = uint8_t((v >> 24) & 0xff);
  };
  buf[0] = 'B';
  buf[1] = 'M';
  put32(2, file_size);
  put32(10, 54);  // pixel data offset
  put32(14, 40);  // DIB header size (BITMAPINFOHEADER)
  put32(18, w);
  put32(22, h);
  put16(26, 1);   // planes
  put16(28, 24);  // bits per pixel
  put32(34, pixel_array);

  const size_t src_stride = image.stride ? image.stride : size_t(w) * 4;
  for (uint32_t y = 0; y < h; ++y) {
    // BMP scanlines are bottom-up.
    const uint8_t* src = image.data.data() + size_t(h - 1 - y) * src_stride;
    uint8_t* dst = buf.data() + 54 + size_t(y) * padded_row;
    for (uint32_t x = 0; x < w; ++x) {
      // src is R,G,B,X; BMP wants B,G,R.
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
  }

  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace

TraceDump::TraceDump() = default;

TraceDump::~TraceDump() = default;

int TraceDump::Main(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    REXGPU_ERROR("usage: trace_dump <trace_file> [output_base] [frame_index]");
    return 5;
  }

  std::filesystem::path path = rex::to_path(args[1]);
  std::filesystem::path output_path;
  if (args.size() >= 3) {
    output_path = rex::to_path(args[2]);
  }
  if (args.size() >= 4) {
    frame_index_ = std::atoi(args[3].c_str());
  }

  auto abs_path = std::filesystem::absolute(path);
  REXGPU_INFO("Loading trace file {} (frame {})...", rex::path_to_utf8(abs_path), frame_index_);

  if (!Setup()) {
    REXGPU_ERROR("Unable to setup trace dump tool");
    return 4;
  }
  if (!Load(abs_path)) {
    REXGPU_ERROR("Unable to load trace file; not found?");
    return 5;
  }

  if (output_path.empty()) {
    output_path = path;
    output_path.replace_extension();
  }
  base_output_path_ = output_path;
  rex::filesystem::CreateParentFolder(base_output_path_);

  return Run();
}

bool TraceDump::Setup() {
  // Headless runtime: empty game_data_root => VFS setup is skipped. No audio,
  // no input, no kernel game-init: trace replay only needs guest memory + the
  // GPU command processor + an offscreen presenter to capture from.
  emulator_ = std::make_unique<Runtime>(std::filesystem::path{});

  RuntimeConfig config;
  config.graphics = CreateGraphicsSystem();
  config.force_presentation = true;  // create an offscreen presenter (no window)

  X_STATUS result = emulator_->Setup(std::move(config));
  if (XFAILED(result)) {
    REXGPU_ERROR("Failed to setup runtime: {:08X}", uint32_t(result));
    return false;
  }

  graphics_system_ = static_cast<GraphicsSystem*>(emulator_->graphics_system());
  if (!graphics_system_) {
    REXGPU_ERROR("Runtime has no graphics system");
    return false;
  }
  player_ = std::make_unique<TracePlayer>(graphics_system_);
  return true;
}

bool TraceDump::Load(const std::filesystem::path& trace_file_path) {
  trace_file_path_ = trace_file_path;
  if (!player_->Open(rex::path_to_utf8(trace_file_path_))) {
    REXGPU_ERROR("Could not load trace file");
    return false;
  }
  return true;
}

int TraceDump::Run() {
  BeginHostCapture();
  player_->SeekFrame(frame_index_);
  const auto* frame = player_->current_frame();
  if (!frame) {
    REXGPU_ERROR("Frame {} is out of range", frame_index_);
    EndHostCapture();
    return 6;
  }
  player_->SeekCommand(static_cast<int>(frame->commands.size()) - 1);
  player_->WaitOnPlayback();
  EndHostCapture();

  int result = 0;
  ui::Presenter* presenter = graphics_system_->presenter();
  ui::RawImage image;
  if (presenter && presenter->CaptureGuestOutput(image)) {
    std::filesystem::path bmp_path = base_output_path_;
    bmp_path += ".bmp";
    if (WriteBmp(bmp_path, image)) {
      REXGPU_INFO("Wrote {} ({}x{})", rex::path_to_utf8(bmp_path), image.width, image.height);
    } else {
      REXGPU_ERROR("Failed to write BMP output");
      result = 1;
    }
    // Raw RGBX for byte-exact backend diffing.
    std::filesystem::path raw_path = base_output_path_;
    raw_path += ".rgba";
    if (FILE* f = std::fopen(raw_path.string().c_str(), "wb")) {
      std::fwrite(image.data.data(), 1, image.data.size(), f);
      std::fclose(f);
    }
  } else {
    REXGPU_ERROR("CaptureGuestOutput failed (no presenter, or backend capture unimplemented)");
    result = 1;
  }

  // Backend-agnostic capture: dump the resolved swap surface straight from guest
  // memory. The headless offscreen presenter can't always capture, and reading
  // guest memory works for ANY backend once the resolve path writes pixels. The
  // swap frontbuffer for this title's menu trace is at 0x1EFC8000, 1280x720.
  // Guest swap data is BGRA and may be Xenos-tiled; this dumps raw bytes as
  // linear (channels/tiling may be scrambled) purely to reveal whether the
  // resolve produced real STRUCTURE vs. an empty surface.
  if (auto* mem = graphics_system_->memory()) {
    constexpr uint32_t kSwapAddr = 0x1EFC8000u;
    constexpr uint32_t kSwapW = 1280u;
    constexpr uint32_t kSwapH = 720u;
    if (const uint8_t* src = mem->TranslatePhysical<const uint8_t*>(kSwapAddr)) {
      ui::RawImage gm;
      gm.width = kSwapW;
      gm.height = kSwapH;
      gm.stride = size_t(kSwapW) * 4;
      gm.data.assign(src, src + gm.stride * size_t(kSwapH));
      uint64_t nonzero = 0;
      for (size_t i = 0; i + 3 < gm.data.size(); i += 4) {
        if (gm.data[i] | gm.data[i + 1] | gm.data[i + 2]) {
          ++nonzero;
        }
      }
      std::filesystem::path gm_bmp = base_output_path_;
      gm_bmp += ".guestmem.bmp";
      WriteBmp(gm_bmp, gm);
      std::filesystem::path gm_raw = base_output_path_;
      gm_raw += ".guestmem.rgba";
      if (FILE* f = std::fopen(gm_raw.string().c_str(), "wb")) {
        std::fwrite(gm.data.data(), 1, gm.data.size(), f);
        std::fclose(f);
      }
      REXGPU_INFO("guestmem swap dump 0x{:08X} {}x{}: {} nonzero px -> {}", kSwapAddr, kSwapW, kSwapH,
                  nonzero, rex::path_to_utf8(gm_bmp));
    }
  }

  player_.reset();
  emulator_.reset();
  return result;
}

}  // namespace rex::graphics
