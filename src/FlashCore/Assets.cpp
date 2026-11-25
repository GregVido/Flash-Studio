#include "Binary.hpp"
#include "Compression.hpp"

#include <Windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>

#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace flash
{
Asset export_shape(unsigned code, ByteView payload, const Movie* movie);

namespace
{
constexpr std::size_t max_pixels = 16'777'216;

void be32(Bytes& output, std::uint32_t value)
{
    for (unsigned shift : {24, 16, 8, 0})
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void png_chunk(Bytes& png, std::string_view name, ByteView data)
{
    be32(png, static_cast<std::uint32_t>(data.size()));
    const auto start = png.size();
    png.insert(png.end(), name.begin(), name.end());
    append(png, data);
    static constexpr auto table = []
    {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i)
        {
            auto value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (value & 1 ? 0xEDB88320u : 0);
            result[i] = value;
        }
        return result;
    }();
    auto crc = 0xFFFFFFFFu;
    for (const auto byte : ByteView{png}.subspan(start))
        crc = table[(crc ^ byte) & 255] ^ (crc >> 8);
    be32(png, ~crc);
}

std::size_t pixel_count(unsigned width, unsigned height)
{
    if (!width || !height || width > max_pixels / height)
        throw std::runtime_error("Dimensions d’image invalides ou supérieures à 16 mégapixels.");
    return static_cast<std::size_t>(width) * height;
}

void validate_scale(double scale)
{
    if (!std::isfinite(scale) || scale < 0.125 || scale > 8)
        throw std::runtime_error("L’échelle doit être comprise entre 12,5 % et 800 %.");
}

Asset resize_svg(const Asset& image, double scale)
{
    if (scale == 1)
        return image;
    std::string svg(image.bytes.begin(), image.bytes.end());
    for (const std::string_view attribute : {" width=\"", " height=\""})
    {
        const auto at = svg.find(attribute);
        if (at == std::string::npos || at > svg.find('>'))
            throw std::runtime_error("Dimensions du SVG introuvables.");
        const auto first = at + attribute.size(), last = svg.find('"', first);
        if (last == std::string::npos)
            throw std::runtime_error("Dimensions du SVG introuvables.");
        double size{};
        const auto parsed = std::from_chars(svg.data() + first, svg.data() + last, size);
        if (parsed.ec != std::errc{} || parsed.ptr != svg.data() + last || !std::isfinite(size) || size <= 0)
            throw std::runtime_error("Dimensions du SVG non numériques.");
        svg.replace(first, last - first, std::format("{:.12g}", size * scale));
    }
    return {L".svg", Bytes(svg.begin(), svg.end())};
}

Bytes png_scanlines(unsigned width, unsigned height, ByteView rgba)
{
    Bytes lines;
    lines.reserve(rgba.size() + height);
    const auto row_size = static_cast<std::size_t>(width) * 4;
    for (unsigned y = 0; y < height; ++y)
    {
        lines.push_back(0);
        append(lines, rgba.subspan(row_size * y, row_size));
    }
    return deflate_zlib(lines);
}

Bytes png_header(unsigned width, unsigned height)
{
    Bytes png{137, 80, 78, 71, 13, 10, 26, 10}, header;
    be32(header, width);
    be32(header, height);
    append(header, std::array<std::uint8_t, 5>{8, 6, 0, 0, 0});
    png_chunk(png, "IHDR", header);
    return png;
}

Asset png_from_rgba(unsigned width, unsigned height, ByteView rgba)
{
    auto png = png_header(width, height);
    png_chunk(png, "IDAT", png_scanlines(width, height, rgba));
    png_chunk(png, "IEND", {});
    return {L".png", std::move(png)};
}

std::uint8_t unpremultiply(unsigned color, unsigned alpha)
{
    return alpha ? static_cast<std::uint8_t>((std::min)(255u, (color * 255 + alpha / 2) / alpha)) : 0;
}

Asset lossless_bitmap(ByteView payload, bool alpha)
{
    Reader input(payload);
    input.skip(2);
    const auto format = input.u8();
    const auto width = input.u16(), height = input.u16();
    const auto pixels = pixel_count(width, height);
    const auto color_count = format == 3 ? input.u8() + 1u : 0u;
    if (format != 3 && format != 5 && (format != 4 || alpha))
        throw std::runtime_error("Format bitmap SWF non reconnu.");
    const auto palette_size = color_count * (alpha ? 4u : 3u);
    const auto row_bytes = width * (format == 3 ? 1u : format == 4 ? 2u : 4u);
    const auto stride = (row_bytes + 3u) & ~3u;
    const auto decoded = inflate_zlib(input.take(input.remaining()), palette_size + static_cast<std::size_t>(height) * stride);
    Bytes rgba;
    rgba.reserve(pixels * 4);
    for (unsigned y = 0; y < height; ++y)
    {
        Reader row(ByteView{decoded}.subspan(palette_size + y * stride, row_bytes));
        for (unsigned x = 0; x < width; ++x)
        {
            if (format == 3)
            {
                const auto id = row.u8();
                if (id >= color_count)
                    throw std::runtime_error("Indice hors de la palette SWF.");
                const auto index = id * (alpha ? 4u : 3u);
                // In paletted lossless images the palette contains RGBA entries.
                append(rgba, ByteView{decoded}.subspan(index, 3));
                rgba.push_back(alpha ? decoded[index + 3] : 255);
            }
            else if (format == 4)
            {
                const auto high = row.u8();
                const auto rgb = (static_cast<unsigned>(high) << 8) | row.u8();
                for (unsigned shift : {10, 5, 0})
                {
                    const auto color = (rgb >> shift) & 31;
                    rgba.push_back(static_cast<std::uint8_t>((color << 3) | (color >> 2)));
                }
                rgba.push_back(255);
            }
            else
            {
                const auto opacity = row.u8();
                for (unsigned channel = 0; channel < 3; ++channel)
                {
                    const auto color = row.u8();
                    rgba.push_back(alpha ? unpremultiply(color, opacity) : color);
                }
                rgba.push_back(alpha ? opacity : 255);
            }
        }
    }
    return png_from_rgba(width, height, rgba);
}

ByteView normalized_jpeg(ByteView data)
{
    if (data.size() >= 4 && data[0] == 255 && data[1] == 217 && data[2] == 255 && data[3] == 216)
        return data.subspan(2);
    return data;
}

Asset encoded_bitmap(ByteView bytes)
{
    auto extension = L".jpg";
    if (bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
        extension = L".png";
    else if (bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F')
        extension = L".gif";
    else
    {
        bytes = normalized_jpeg(bytes);
        if (bytes.size() < 2 || bytes[0] != 255 || bytes[1] != 216)
            throw std::runtime_error("Format d’image incorporée non reconnu.");
        // Some early encoders also inserted EOI/SOI after the first SOI.
        if (bytes.size() >= 6 && bytes[2] == 255 && bytes[3] == 217 && bytes[4] == 255 && bytes[5] == 216)
            bytes = bytes.subspan(4);
    }
    return {extension, Bytes(bytes.begin(), bytes.end())};
}

void check_image(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("Impossible de décoder l’image incorporée.");
}

struct ImageApartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ImageApartment() { if (result != RPC_E_CHANGED_MODE) check_image(result); }
    ~ImageApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

using Microsoft::WRL::ComPtr;

ComPtr<ID2D1DeviceContext5> svg_context()
{
    ComPtr<ID3D11Device> device;
    check_image(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    ComPtr<IDXGIDevice> dxgi;
    check_image(device.As(&dxgi));
    ComPtr<ID2D1Factory1> factory;
    check_image(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
    ComPtr<ID2D1Device> graphics;
    check_image(factory->CreateDevice(dxgi.Get(), &graphics));
    ComPtr<ID2D1DeviceContext> context;
    check_image(graphics->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
    ComPtr<ID2D1DeviceContext5> svg;
    check_image(context.As(&svg));
    return svg;
}

struct Raster
{
    unsigned width, height;
    Bytes rgba;
};

Raster rasterize(const Asset& asset, double scale)
{
    const ImageApartment apartment;
    thread_local ComPtr<ID2D1DeviceContext5> context;
    if (!context)
        context = svg_context();
    ComPtr<IStream> input;
    check_image(CreateStreamOnHGlobal(nullptr, TRUE, &input));
    check_image(input->Write(asset.bytes.data(), static_cast<ULONG>(asset.bytes.size()), nullptr));
    check_image(input->Seek({}, STREAM_SEEK_SET, nullptr));
    ComPtr<ID2D1SvgDocument> document;
    check_image(context->CreateSvgDocument(input.Get(), D2D1::SizeF(1, 1), &document));
    ComPtr<ID2D1SvgElement> root;
    document->GetRoot(&root);
    D2D1_SVG_LENGTH width{}, height{};
    check_image(root->GetAttributeValue(L"width", &width));
    check_image(root->GetAttributeValue(L"height", &height));
    const auto measure = [scale](const D2D1_SVG_LENGTH& length)
    {
        const auto pixels = length.value * scale;
        if (length.units != D2D1_SVG_LENGTH_UNITS_NUMBER || !std::isfinite(pixels) || pixels <= 0 || pixels > 8192)
            throw std::runtime_error("Dimensions SVG incompatibles avec la taille d’export.");
        return static_cast<unsigned>(std::ceil(pixels));
    };
    const auto w = measure(width), h = measure(height);
    Bytes rgba(pixel_count(w, h) * 4);
    check_image(document->SetViewportSize(D2D1::SizeF(width.value, height.value)));

    const auto size = D2D1::SizeU(w, h);
    const auto pixel = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const auto target_properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixel, 96, 96);
    ComPtr<ID2D1Bitmap1> target;
    check_image(context->CreateBitmap(size, nullptr, 0, &target_properties, &target));
    context->SetTarget(target.Get());
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(scale), static_cast<float>(scale)));
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    context->DrawSvgDocument(document.Get());
    const auto drawn = context->EndDraw();
    context->SetTarget(nullptr);
    if (drawn == D2DERR_RECREATE_TARGET)
        context.Reset();
    check_image(drawn);

    ComPtr<ID2D1Bitmap1> readable;
    const auto read_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pixel, 96, 96);
    check_image(context->CreateBitmap(size, nullptr, 0, &read_properties, &readable));
    check_image(readable->CopyFromBitmap(nullptr, target.Get(), nullptr));
    D2D1_MAPPED_RECT mapped{};
    check_image(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
        {
            const auto* bgra = mapped.bits + static_cast<std::size_t>(y) * mapped.pitch + x * 4;
            const auto at = (static_cast<std::size_t>(y) * w + x) * 4;
            const auto alpha = bgra[3];
            rgba[at] = unpremultiply(bgra[2], alpha);
            rgba[at + 1] = unpremultiply(bgra[1], alpha);
            rgba[at + 2] = unpremultiply(bgra[0], alpha);
            rgba[at + 3] = alpha;
        }
    check_image(readable->Unmap());
    return {w, h, std::move(rgba)};
}

ComPtr<IWICImagingFactory> image_factory()
{
    ComPtr<IWICImagingFactory> factory;
    check_image(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    return factory;
}

std::size_t stream_size(IStream& stream)
{
    STATSTG info{};
    check_image(stream.Stat(&info, STATFLAG_NONAME));
    if (info.cbSize.QuadPart > max_file_size)
        throw std::runtime_error("Image exportée trop volumineuse.");
    return static_cast<std::size_t>(info.cbSize.QuadPart);
}

Bytes read_image_stream(IStream& stream)
{
    Bytes bytes(stream_size(stream));
    check_image(stream.Seek({}, STREAM_SEEK_SET, nullptr));
    ULONG read{};
    check_image(stream.Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
    if (read != bytes.size())
        throw std::runtime_error("Lecture incomplète de l’image exportée.");
    return bytes;
}

class ApngAnimation
{
public:
    ApngAnimation(unsigned frames, double fps) : frames_(frames)
    {
        const auto rate = static_cast<unsigned>(std::lround(fps * 256));
        const auto divisor = std::gcd(256u, rate);
        delay_ = static_cast<std::uint16_t>(256 / divisor);
        units_ = static_cast<std::uint16_t>(rate / divisor);
    }

    void add(const Raster& raster)
    {
        if (png_.empty())
        {
            png_ = png_header(raster.width, raster.height);
            Bytes animation;
            be32(animation, frames_);
            be32(animation, 0);
            png_chunk(png_, "acTL", animation);
        }
        Bytes control;
        be32(control, sequence_++);
        be32(control, raster.width);
        be32(control, raster.height);
        be32(control, 0);
        be32(control, 0);
        append(control, std::array{static_cast<std::uint8_t>(delay_ >> 8), static_cast<std::uint8_t>(delay_),
            static_cast<std::uint8_t>(units_ >> 8), static_cast<std::uint8_t>(units_), std::uint8_t{0}, std::uint8_t{0}});
        png_chunk(png_, "fcTL", control);

        const auto pixels = png_scanlines(raster.width, raster.height, raster.rgba);
        if (sequence_ == 1)
            png_chunk(png_, "IDAT", pixels);
        else
        {
            Bytes data;
            data.reserve(4 + pixels.size());
            be32(data, sequence_++);
            append(data, pixels);
            png_chunk(png_, "fdAT", data);
        }
        if (png_.size() > max_file_size - 12)
            throw std::runtime_error("Le PNG animé dépasse 256 Mio.");
    }

    Bytes finish()
    {
        png_chunk(png_, "IEND", {});
        return std::move(png_);
    }

private:
    unsigned frames_, sequence_ = 0;
    std::uint16_t delay_{}, units_{};
    Bytes png_;
};

class GifAnimation
{
public:
    GifAnimation() : factory_(image_factory())
    {
        check_image(CreateStreamOnHGlobal(nullptr, TRUE, &stream_));
        check_image(factory_->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder_));
        check_image(encoder_->Initialize(stream_.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICPalette> background;
        check_image(factory_->CreatePalette(&background));
        WICColor colors[]{0, 0xFF000000};
        check_image(background->InitializeCustom(colors, 2));
        check_image(encoder_->SetPalette(background.Get()));
    }

    void add(Raster raster, std::uint16_t delay)
    {
        ComPtr<IWICBitmap> pixels;
        check_image(factory_->CreateBitmapFromMemory(raster.width, raster.height, GUID_WICPixelFormat32bppRGBA,
            raster.width * 4, static_cast<UINT>(raster.rgba.size()), raster.rgba.data(), &pixels));
        ComPtr<IWICPalette> palette;
        check_image(factory_->CreatePalette(&palette));
        check_image(palette->InitializeFromBitmap(pixels.Get(), 255, FALSE));
        std::array<WICColor, 256> colors{};
        UINT count{};
        check_image(palette->GetColors(255, colors.data() + 1, &count));
        for (unsigned i = 1; i <= count; ++i)
            colors[i] |= 0xFF000000;
        check_image(palette->InitializeCustom(colors.data(), count + 1));
        ComPtr<IWICFormatConverter> converter;
        check_image(factory_->CreateFormatConverter(&converter));
        check_image(converter->Initialize(pixels.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone,
            palette.Get(), 50.0, WICBitmapPaletteTypeCustom));
        Bytes indexed(pixel_count(raster.width, raster.height));
        check_image(converter->CopyPixels(nullptr, raster.width, static_cast<UINT>(indexed.size()), indexed.data()));

        ComPtr<IWICBitmapFrameEncode> frame;
        check_image(encoder_->CreateNewFrame(&frame, nullptr));
        check_image(frame->Initialize(nullptr));
        check_image(frame->SetSize(raster.width, raster.height));
        auto pixel_format = GUID_WICPixelFormat8bppIndexed;
        check_image(frame->SetPixelFormat(&pixel_format));
        check_image(frame->SetPalette(palette.Get()));
        ComPtr<IWICMetadataQueryWriter> metadata;
        check_image(frame->GetMetadataQueryWriter(&metadata));
        PROPVARIANT value{};
        value.vt = VT_UI2;
        value.uiVal = delay;
        check_image(metadata->SetMetadataByName(L"/grctlext/Delay", &value));
        value.vt = VT_UI1;
        value.bVal = 2;
        check_image(metadata->SetMetadataByName(L"/grctlext/Disposal", &value));
        value.bVal = 0;
        check_image(metadata->SetMetadataByName(L"/grctlext/TransparentColorIndex", &value));
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        check_image(metadata->SetMetadataByName(L"/grctlext/TransparencyFlag", &value));
        check_image(frame->WritePixels(raster.height, raster.width, static_cast<UINT>(indexed.size()), indexed.data()));
        check_image(frame->Commit());
        (void)stream_size(*stream_.Get());
    }

    Bytes finish()
    {
        check_image(encoder_->Commit());
        auto bytes = read_image_stream(*stream_.Get());
        // WIC writes the frames and their palettes. Add the GIF looping application extension.
        const auto flags = bytes.at(10);
        const auto first_block = 13 + (flags & 128 ? 3 * (2 << (flags & 7)) : 0);
        constexpr std::uint8_t loop[]{0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0};
        bytes.insert(bytes.begin() + first_block, std::begin(loop), std::end(loop));
        return bytes;
    }

private:
    ImageApartment apartment_;
    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IStream> stream_;
    ComPtr<IWICBitmapEncoder> encoder_;
};

class SvgAnimation
{
public:
    SvgAnimation(unsigned frames, double fps) : frames_(frames), seconds_(frames / fps) {}

    void add(unsigned frame, const Asset& asset)
    {
        const std::string_view svg(reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size());
        const auto header = svg.find('>') + 1;
        if (frame == 1)
            text_ = svg.substr(0, header);
        text_ += frame == 1 ? "\n<g visibility=\"visible\">\n" : "\n<g visibility=\"hidden\">\n";
        if (frames_ > 1)
        {
            const auto start = static_cast<double>(frame - 1) / frames_;
            const auto end = static_cast<double>(frame) / frames_;
            const auto keys = frame == 1 ? std::format("0;{:.12g};1", end) : frame == frames_ ?
                std::format("0;{:.12g};1", start) : std::format("0;{:.12g};{:.12g};1", start, end);
            const auto values = frame == 1 ? "visible;hidden;hidden" : frame == frames_ ?
                "hidden;visible;visible" : "hidden;visible;hidden;hidden";
            text_ += std::format("<animate attributeName=\"visibility\" calcMode=\"discrete\" dur=\"{:.12g}s\" "
                "repeatCount=\"indefinite\" keyTimes=\"{}\" values=\"{}\"/>\n", seconds_, keys, values);
        }
        std::string contents(svg.substr(header, svg.rfind("</svg>") - header));
        const auto prefix = std::format("frame{}-", frame);
        for (const std::string_view token : {" id=\"", "url(#", "href=\"#"})
            for (auto at = contents.find(token); at != std::string::npos; at = contents.find(token, at + token.size() + prefix.size()))
                contents.insert(at + token.size(), prefix);
        if (text_.size() + contents.size() + 12 > max_file_size)
            throw std::runtime_error("Le SVG animé dépasse 256 Mio.");
        text_ += contents;
        text_ += "</g>\n";
    }

    Bytes finish()
    {
        text_ += "</svg>\n";
        return Bytes(text_.begin(), text_.end());
    }

private:
    unsigned frames_;
    double seconds_;
    std::string text_;
};

ComPtr<IWICBitmapFrameDecode> image_frame(IWICImagingFactory& factory, ByteView image)
{
    ComPtr<IWICStream> stream;
    check_image(factory.CreateStream(&stream));
    // The decoder only reads from this stream. WIC's memory-stream API takes a mutable pointer.
    check_image(stream->InitializeFromMemory(const_cast<BYTE*>(image.data()), static_cast<DWORD>(image.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    check_image(factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    check_image(decoder->GetFrame(0, &frame));
    return frame;
}

Asset bitmap_with_alpha(Bytes image, ByteView compressed_alpha)
{
    const ImageApartment apartment;
    const auto factory = image_factory();
    const auto frame = image_frame(*factory.Get(), image);
    UINT width{}, height{};
    check_image(frame->GetSize(&width, &height));
    const auto count = pixel_count(width, height);
    const auto alpha = inflate_zlib(compressed_alpha, count);
    ComPtr<IWICFormatConverter> converter;
    check_image(factory->CreateFormatConverter(&converter));
    check_image(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    Bytes rgba(count * 4);
    check_image(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    for (std::size_t i = 0; i < count; ++i)
        rgba[i * 4 + 3] = alpha[i];
    return png_from_rgba(width, height, rgba);
}

void ascii(Bytes& output, std::string_view value) { output.insert(output.end(), value.begin(), value.end()); }

std::optional<Asset> sound(ByteView payload)
{
    Reader input(payload);
    input.skip(2);
    const auto settings = input.u8();
    const auto sample_count = input.u32();
    const auto codec = settings >> 4;
    if (codec == 2)
    {
        input.skip(2);
        const auto data = input.take(input.remaining());
        return Asset{L".mp3", Bytes(data.begin(), data.end())};
    }
    if (codec != 0 && codec != 3)
        return {};
    constexpr unsigned rates[]{5512, 11025, 22050, 44100};
    const auto channels = settings & 1 ? 2u : 1u;
    const auto sample_size = settings & 2 ? 2u : 1u;
    const auto rate = rates[(settings >> 2) & 3];
    const auto block = sample_size * channels;
    if (sample_count > input.remaining() / block)
        throw std::runtime_error("Échantillons PCM tronqués.");
    const auto data_size = sample_count * block;
    Bytes wave;
    ascii(wave, "RIFF"); put32(wave, 36 + data_size + (data_size & 1));
    ascii(wave, "WAVEfmt "); put32(wave, 16);
    put16(wave, 1); put16(wave, static_cast<std::uint16_t>(channels));
    put32(wave, rate); put32(wave, rate * block);
    put16(wave, static_cast<std::uint16_t>(block)); put16(wave, static_cast<std::uint16_t>(sample_size * 8));
    ascii(wave, "data"); put32(wave, data_size);
    append(wave, input.take(data_size));
    if (data_size & 1)
        wave.push_back(0);
    return Asset{L".wav", std::move(wave)};
}
}

std::optional<Asset> extract(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    const auto payload = movie.data(index);
    Reader input(payload);
    switch (code)
    {
    case 2: case 22: case 32: case 83:
        return export_shape(code, payload, &movie);
    case 6:
    {
        input.skip(2);
        auto bits = normalized_jpeg(input.take(input.remaining()));
        for (std::size_t i = 0; i < movie.tags().size(); ++i)
        {
            if (movie.tags()[i].code != 8)
                continue;
            auto tables = movie.data(i);
            if (tables.size() >= 2 && tables[tables.size() - 2] == 255 && tables.back() == 217)
                tables = tables.first(tables.size() - 2);
            if (tables.empty())
                break;
            if (bits.size() >= 2 && bits[0] == 255 && bits[1] == 216)
                bits = bits.subspan(2);
            Bytes jpeg(tables.begin(), tables.end());
            append(jpeg, bits);
            return encoded_bitmap(jpeg);
        }
        return encoded_bitmap(bits);
    }
    case 14:
        return sound(payload);
    case 11: case 33: case 37:
    {
        const auto text = extract_text(movie, index);
        return Asset{L".txt", Bytes(text.begin(), text.end())};
    }
    case 20: case 36:
        return lossless_bitmap(payload, code == 36);
    case 21:
        input.skip(2);
        return encoded_bitmap(input.take(input.remaining()));
    case 35: case 90:
    {
        input.skip(2);
        const auto offset = input.u32();
        if (code == 90)
            input.skip(2);
        auto image = encoded_bitmap(input.take(offset));
        if (!input.remaining() || image.extension != L".jpg")
            return image;
        return bitmap_with_alpha(std::move(image.bytes), input.take(input.remaining()));
    }
    case 39:
        return render_frame(movie, 1, index).image;
    case 72: case 82:
        if (code == 82)
        {
            input.skip(4);
            input.cstring();
        }
        return Asset{L".abc", Bytes(payload.begin() + input.position(), payload.end())};
    case 77:
    {
        auto body = input.cstring();
        return Asset{L".xml", Bytes(body.begin(), body.end())};
    }
    case 87:
        input.skip(6);
        return Asset{L".bin", Bytes(payload.begin() + input.position(), payload.end())};
    case 91:
    {
        input.skip(2);
        const auto flags = input.u8();
        input.cstring();
        if (!(flags & 4) || !input.remaining())
            return {};
        const auto font = input.take(input.remaining());
        const bool opentype = font.size() >= 4 &&
            font[0] == 'O' && font[1] == 'T' && font[2] == 'T' && font[3] == 'O';
        return Asset{opentype ? L".otf" : L".cff", Bytes(font.begin(), font.end())};
    }
    default:
        return {};
    }
}

bool is_visual_tag(unsigned code) noexcept
{
    switch (code)
    {
    case 2: case 6: case 11: case 20: case 21: case 22: case 32: case 33: case 35: case 36: case 39: case 83: case 90:
        return true;
    default:
        return false;
    }
}

bool is_bitmap_tag(unsigned code) noexcept
{
    return code == 6 || code == 20 || code == 21 || code == 35 || code == 36 || code == 90;
}

Movie Movie::replace_image(std::size_t index, ByteView encoded) const
{
    if (!is_bitmap_tag(tags().at(index).code) || !tags()[index].character_id)
        throw std::runtime_error("Cette ressource n’est pas une image bitmap.");
    const ImageApartment apartment;
    const auto factory = image_factory();
    const auto frame = image_frame(*factory.Get(), encoded);
    UINT width{}, height{};
    check_image(frame->GetSize(&width, &height));
    const auto count = pixel_count(width, height);
    if (width > UINT16_MAX || height > UINT16_MAX)
        throw std::runtime_error("Dimensions trop grandes pour une image SWF.");
    const bool alpha = header().version >= 3;
    ComPtr<IWICFormatConverter> converter;
    check_image(factory->CreateFormatConverter(&converter));
    check_image(converter->Initialize(frame.Get(), alpha ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    Bytes pixels(count * 4);
    check_image(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        // SWF stores 32-bit lossless pixels as ARGB or XRGB.
        std::swap(pixels[offset], pixels[offset + 3]);
        std::swap(pixels[offset + 1], pixels[offset + 2]);
        if (!alpha)
            pixels[offset] = 0;
    }
    Bytes payload;
    put16(payload, static_cast<std::uint16_t>(*tags()[index].character_id));
    payload.push_back(5);
    put16(payload, static_cast<std::uint16_t>(width));
    put16(payload, static_cast<std::uint16_t>(height));
    append(payload, deflate_zlib(pixels));
    return replace_tag(index, payload, alpha ? 36u : 20u);
}

Movie Movie::replace_sound(std::size_t index, const std::filesystem::path& audio) const
{
    const auto& tag = tags().at(index);
    if (tag.code != 14 || !tag.character_id)
        throw std::runtime_error("Cette ressource n’est pas un son indépendant.");
    if (std::filesystem::file_size(audio) > max_file_size)
        throw std::runtime_error("Le fichier audio dépasse 256 Mio.");
    const auto check_audio = [](HRESULT result)
    {
        if (FAILED(result))
            throw std::runtime_error(std::format("Impossible de décoder le fichier audio (0x{:08X}).",
                static_cast<unsigned>(result)));
    };
    const ImageApartment apartment;
    check_audio(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    struct MediaStartup { ~MediaStartup() { MFShutdown(); } };
    const MediaStartup startup;
    ComPtr<IMFSourceReader> reader;
    check_audio(MFCreateSourceReaderFromURL(audio.c_str(), nullptr, &reader));
    constexpr auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    check_audio(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    check_audio(reader->SetStreamSelection(stream, TRUE));
    ComPtr<IMFMediaType> input, output;
    check_audio(reader->GetNativeMediaType(stream, 0, &input));
    UINT32 native_rate{}, native_channels{};
    check_audio(input->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &native_rate));
    check_audio(input->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &native_channels));
    constexpr std::array<UINT32, 4> rates{5512, 11025, 22050, 44100};
    const auto rate_index = std::ranges::find(rates, native_rate);
    const auto rate = rate_index == rates.end() ? rates.back() : *rate_index;
    const auto channels = native_channels == 1 ? 1u : 2u;
    const auto block_size = channels * 2;
    check_audio(MFCreateMediaType(&output));
    check_audio(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_audio(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    check_audio(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    check_audio(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate));
    check_audio(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    check_audio(output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_size));
    check_audio(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * block_size));
    check_audio(reader->SetCurrentMediaType(stream, nullptr, output.Get()));

    Bytes payload;
    put16(payload, static_cast<std::uint16_t>(*tag.character_id));
    payload.push_back(static_cast<std::uint8_t>(0x32 | ((std::ranges::find(rates, rate) - rates.begin()) << 2) |
        (channels == 2 ? 1 : 0)));
    put32(payload, 0);
    for (unsigned empty_samples = 0;;)
    {
        ComPtr<IMFSample> sample;
        DWORD flags{};
        check_audio(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample));
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
            throw std::runtime_error("Le format audio change au cours du fichier.");
        if (sample)
        {
            ComPtr<IMFMediaBuffer> buffer;
            check_audio(sample->ConvertToContiguousBuffer(&buffer));
            BYTE* bytes{};
            DWORD size{};
            check_audio(buffer->Lock(&bytes, nullptr, &size));
            try { if (size) append(payload, {bytes, size}); }
            catch (...) { buffer->Unlock(); throw; }
            check_audio(buffer->Unlock());
            if (size)
                empty_samples = 0;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (++empty_samples >= 1024)
            throw std::runtime_error("Le fichier audio ne fournit pas d’échantillons.");
    }
    const auto pcm_size = payload.size() - 7;
    if (!pcm_size || pcm_size % block_size)
        throw std::runtime_error("Échantillons PCM invalides.");
    const auto samples = pcm_size / block_size;
    for (unsigned i = 0; i < 4; ++i)
        payload[3 + i] = static_cast<std::uint8_t>(samples >> (8 * i));
    return replace_tag(index, payload);
}

std::string binary_preview(const Movie& movie, std::size_t index)
{
    const auto tag = movie.tags().at(index);
    if (tag.code != 87 || tag.size < 6)
        throw std::runtime_error("Ce tag ne contient pas de données binaires.");
    const auto data = movie.data(index).subspan(6);
    auto summary = std::format("DefineBinaryData #{} · {} octets\n", tag.character_id.value_or(0), data.size());
    try
    {
        std::optional<Movie> nested;
        if (data.size() >= 8 && (data[0] == 'F' || data[0] == 'C' || data[0] == 'Z') && data[1] == 'W' && data[2] == 'S')
            nested = Movie::parse(data);
        else if (data.size() >= 8 && data[0] >= 6 && data[0] <= 50 && data[5] == 0x78 &&
            ((static_cast<unsigned>(data[5]) << 8) + data[6]) % 31 == 0)
        {
            // Some loaders prepend only the SWF signature to an otherwise complete compressed file.
            Bytes reconstructed{'C', 'W', 'S'};
            append(reconstructed, data);
            nested = Movie::parse(reconstructed);
        }
        if (nested)
            summary += '\n' + summarize(*nested) + '\n';
    }
    catch (const std::runtime_error&) {}
    return summary + '\n' + hex_dump(data, 64 * 1024);
}

std::wstring_view animation_extension(AnimationFormat format) noexcept
{
    switch (format)
    {
    case AnimationFormat::gif: return L".gif";
    case AnimationFormat::apng: return L".apng";
    case AnimationFormat::svg: return L".svg";
    }
    return {};
}

SpriteAnimation export_sprite_animation(const Movie& movie, std::size_t sprite, AnimationFormat format, double scale)
{
    validate_scale(scale);
    const auto count = (std::max)(1u, frame_count(movie, sprite));
    const auto fps = playback_fps(movie);
    SpriteAnimation result{{std::wstring(animation_extension(format)), {}}};
    const auto encode = [&](auto&& add_frame)
    {
        render_sprite_frames(movie, sprite, [&](unsigned frame, FramePreview preview)
        {
            result.frames_with_omissions += preview.omitted != 0;
            add_frame(frame, preview.image);
        });
    };
    if (format == AnimationFormat::svg)
    {
        SvgAnimation animation(count, fps);
        encode([&](unsigned frame, const Asset& image) { animation.add(frame, image); });
        result.image = resize_svg({L".svg", animation.finish()}, scale);
    }
    else if (format == AnimationFormat::apng)
    {
        ApngAnimation animation(count, fps);
        encode([&](unsigned, const Asset& image) { animation.add(rasterize(image, scale)); });
        result.image.bytes = animation.finish();
    }
    else
    {
        GifAnimation animation;
        encode([&](unsigned frame, const Asset& image)
        {
            const auto end = std::llround(frame * 100.0 / fps);
            const auto start = std::llround((frame - 1) * 100.0 / fps);
            const auto delay = static_cast<std::uint16_t>(std::clamp(end - start, 1ll, 65535ll));
            animation.add(rasterize(image, scale), delay);
        });
        result.image.bytes = animation.finish();
    }
    return result;
}

Asset visual_asset(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    if (code == 11 || code == 33)
        return text_svg(movie, index);
    if (is_visual_tag(code))
        if (auto asset = extract(movie, index))
            return std::move(*asset);
    throw std::runtime_error("Ce tag ne dispose pas de rendu graphique exportable.");
}

Asset convert_image(const Asset& image, std::wstring_view extension, double scale)
{
    std::wstring format(extension);
    if (!format.empty())
        CharLowerBuffW(format.data(), static_cast<DWORD>(format.size()));
    validate_scale(scale);
    if (format == L".svg" && image.extension == L".svg")
        return resize_svg(image, scale);
    const GUID* container{};
    if (format == L".png")
        container = &GUID_ContainerFormatPng;
    else if (format == L".bmp")
        container = &GUID_ContainerFormatBmp;
    else if (format == L".jpg" || format == L".jpeg")
        container = &GUID_ContainerFormatJpeg;
    else if (format == L".tif" || format == L".tiff")
        container = &GUID_ContainerFormatTiff;
    else
        throw std::runtime_error("Formats disponibles : SVG pour les ressources vectorielles, PNG, BMP, JPEG et TIFF.");
    if (image.extension == L".svg")
    {
        const auto raster = rasterize(image, scale);
        const auto bitmap = png_from_rgba(raster.width, raster.height, raster.rgba);
        return format == L".png" ? bitmap : convert_image(bitmap, format);
    }
    if (image.extension != L".png" && image.extension != L".jpg" && image.extension != L".gif")
        throw std::runtime_error("Le format de la ressource source n’est pas pris en charge.");
    if (image.extension == format && scale == 1)
        return image;

    const ImageApartment apartment;
    const auto factory = image_factory();
    const auto source = image_frame(*factory.Get(), image.bytes);
    UINT width{}, height{};
    check_image(source->GetSize(&width, &height));
    pixel_count(width, height);
    ComPtr<IWICBitmapScaler> resized;
    IWICBitmapSource* pixels = source.Get();
    if (scale != 1)
    {
        if (std::max(width, height) * scale > 8192)
            throw std::runtime_error("Dimensions supérieures à 8192 pixels après mise à l’échelle.");
        width = static_cast<UINT>(std::ceil(width * scale));
        height = static_cast<UINT>(std::ceil(height * scale));
        pixel_count(width, height);
        check_image(factory->CreateBitmapScaler(&resized));
        check_image(resized->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant));
        pixels = resized.Get();
    }

    ComPtr<IStream> stream;
    check_image(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    check_image(factory->CreateEncoder(*container, nullptr, &encoder));
    check_image(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check_image(encoder->CreateNewFrame(&frame, &options));
    if (container == &GUID_ContainerFormatBmp && options)
    {
        PROPBAG2 option{};
        wchar_t property[] = L"EnableV5Header32bppBGRA";
        option.pstrName = property;
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        check_image(options->Write(1, &option, &value));
    }
    check_image(frame->Initialize(options.Get()));
    check_image(frame->SetSize(width, height));
    auto pixel_format = container == &GUID_ContainerFormatJpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    check_image(frame->SetPixelFormat(&pixel_format));
    ComPtr<IWICFormatConverter> converter;
    check_image(factory->CreateFormatConverter(&converter));
    check_image(converter->Initialize(pixels, pixel_format, WICBitmapDitherTypeNone, nullptr, 0,
        WICBitmapPaletteTypeCustom));
    check_image(frame->WriteSource(converter.Get(), nullptr));
    check_image(frame->Commit());
    check_image(encoder->Commit());

    return {std::move(format), read_image_stream(*stream.Get())};
}

EmbeddedBitmap embedded_bitmap(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    if (code != 6 && code != 20 && code != 21 && code != 35 && code != 36 && code != 90)
        throw std::runtime_error("La référence de texture ne désigne pas un bitmap SWF.");
    const auto original = extract(movie, index);
    if (!original)
        throw std::runtime_error("Image incorporée introuvable.");
    const auto png = convert_image(*original, L".png");
    if (png.bytes.size() < 24 || png.bytes[0] != 137 || png.bytes[1] != 'P' ||
        png.bytes[12] != 'I' || png.bytes[13] != 'H' || png.bytes[14] != 'D' || png.bytes[15] != 'R')
        throw std::runtime_error("En-tête PNG incorporé invalide.");
    const auto dimension = [&](std::size_t offset)
    {
        return (unsigned{png.bytes[offset]} << 24) | (unsigned{png.bytes[offset + 1]} << 16) |
            (unsigned{png.bytes[offset + 2]} << 8) | png.bytes[offset + 3];
    };
    const auto width = dimension(16), height = dimension(20);
    pixel_count(width, height);
    return {width, height, "data:image/png;base64," + encode_base64(png.bytes)};
}
}
