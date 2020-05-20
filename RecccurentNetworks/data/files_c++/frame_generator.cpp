/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include <getopt/getopt.h>

#include <utils/Path.h>

#include <backend/PixelBufferDescriptor.h>

#include <filament/Color.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Renderer.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/TransformManager.h>
#include <filament/View.h>

#include <image/ColorTransform.h>
#include <imageio/ImageEncoder.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec4.h>
#include <math/norm.h>

#include <filamentapp/Config.h>
#include <filamentapp/IBL.h>
#include <filamentapp/FilamentApp.h>
#include <filamentapp/MeshAssimp.h>

using namespace filament::math;
using namespace filament;
using namespace filamat;
using namespace utils;
using namespace image;

struct Param {
    std::string name;
    float start = 0.0f;
    float end = 0.0f;
};

const int FRAME_TO_SKIP = 10;

static std::vector<Path> g_filenames;
static std::vector<char> g_materialBuffer;
static Path g_materialPath;
static Path g_paramsPath;
static bool g_lightOn = false;
static bool g_skyboxOn = true;
static Skybox* g_skybox = nullptr;
static int g_materialVariantCount = 1;
static int g_currentFrame = 0;
static std::atomic_int g_savedFrames(0);
static std::vector<Param> g_parameters;
static std::string g_prefix;
static uint32_t g_clearColor = 0x000000;

std::unique_ptr<MeshAssimp> g_meshSet;
static std::map<std::string, MaterialInstance*> g_meshMaterialInstances;
static const Material* g_material = nullptr;
static MaterialInstance* g_materialInstance = nullptr;
static Entity g_light;

static Config g_config;

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
            "SAMPLE_FRAME_GENERATOR tests a material by varying float parameters\n"
            "Usage:\n"
            "    SAMPLE_FRAME_GENERATOR [options] <mesh files (.obj, .fbx)>\n"
            "\n"
            "This tool loads an object, applies the specified material and renders N\n"
            "frames as specified by the -c flag. For each frame rendered, the material\n"
            "parameters are recomputed based on the start and end values specified in the\n"
            "params file (see -p). Each frame is finally saved as a PNG.\n\n"
            "The --params and --material parameters are mandatory.\n\n"
            "Example of a parameters file that varies only the roughness:\n\n"
            "roughness  0.0 1.0\n"
            "metallic   1.0 1.0\n"
            "\n"
            "Options:\n"
            "   --help, -h\n"
            "       Prints this message\n\n"
            "   --api, -a\n"
            "       Specify the backend API: opengl (default) or vulkan\n\n"
            "   --ibl=<path to cmgen IBL>, -i <path>\n"
            "       Applies an IBL generated by cmgen's deploy option\n\n"
            "   --scale=[number], -s [number]\n"
            "       Applies uniform scale\n\n"
            "   --material=<path>, -m <path>\n"
            "       Path to a compiled material file (see matc)\n\n"
            "   --params=<path>, -p <path>\n"
            "       Path to a parameters file\n"
            "       Each line: param_name start end\n\n"
            "   --count=[integer > 0 && <= 256], -c [integer > 0 && <= 256]\n"
            "       Number of material variants to render\n\n"
            "   --light-on, -l\n"
            "       Turn on the directional light\n\n"
            "   --prefix=[prefix], -x [prefix]\n"
            "       Prefix of the output files\n\n"
            "   --skybox-off, -y\n"
            "       Hide the skybox, showing the clear color\n\n"
            "   --clear-color=0xRRGGBB, -b 0xRRGGBB\n"
            "       Set the clear color\n\n"
    );
    const std::string from("SAMPLE_FRAME_GENERATOR");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    std::cout << usage;
}

static int handleCommandLineArgments(int argc, char* argv[], Config* config) {
    static constexpr const char* OPTSTR = "ha:s:li:m:c:p:x:yb:";
    static const struct option OPTIONS[] = {
            { "help",        no_argument,       nullptr, 'h' },
            { "api",         required_argument, nullptr, 'a' },
            { "ibl",         required_argument, nullptr, 'i' },
            { "scale",       required_argument, nullptr, 's' },
            { "material",    required_argument, nullptr, 'm' },
            { "params",      required_argument, nullptr, 'p' },
            { "count",       required_argument, nullptr, 'c' },
            { "light-on",    no_argument,       nullptr, 'l' },
            { "skybox-off",  no_argument,       nullptr, 'y' },
            { "prefix",      required_argument, nullptr, 'x' },
            { "clear-color", required_argument, nullptr, 'b' },
            { nullptr, 0, nullptr, 0 }  // termination of the option list
    };
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'a':
                if (arg == "opengl") {
                    config->backend = Engine::Backend::OPENGL;
                } else if (arg == "vulkan") {
                    config->backend = Engine::Backend::VULKAN;
                } else {
                    std::cerr << "Unrecognized backend. Must be 'opengl'|'vulkan'." << std::endl;
                }
                break;
            case 'i':
                config->iblDirectory = arg;
                break;
            case 's':
                try {
                    config->scale = std::stof(arg);
                } catch (std::invalid_argument& e) {
                    // keep scale of 1.0
                } catch (std::out_of_range& e) {
                    // keep scale of 1.0
                }
                break;
            case 'b':
                try {
                    g_clearColor = (uint32_t) std::stoul(arg, nullptr, 16);
                } catch (std::invalid_argument& e) {
                    // keep default color
                } catch (std::out_of_range& e) {
                    // keep default color
                }
                break;
            case 'm':
                g_materialPath = arg;
                break;
            case 'p':
                g_paramsPath = arg;
                break;
            case 'x':
                g_prefix = arg;
                break;
            case 'l':
                g_lightOn = true;
                break;
            case 'y':
                g_skyboxOn = false;
            case 'c':
                try {
                    g_materialVariantCount = std::min(std::max(1, std::stoi(arg)), 256);
                } catch (std::invalid_argument& e) {
                    // keep count of 1
                } catch (std::out_of_range& e) {
                    // keep count of 1
                }
                break;
        }
    }

    return optind;
}

static void cleanup(Engine* engine, View*, Scene*) {
    for (auto material : g_meshMaterialInstances) {
        engine->destroy(material.second);
    }

    if (g_skybox) {
        engine->destroy(g_skybox);
    }

    engine->destroy(g_materialInstance);
    engine->destroy(g_material);

    g_meshSet.reset(nullptr);

    engine->destroy(g_light);
    EntityManager& em = EntityManager::get();
    em.destroy(g_light);
}

static std::ifstream::pos_type getFileSize(const char* filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

static void readMaterial(Engine* engine) {
    long fileSize = static_cast<long>(getFileSize(g_materialPath.c_str()));
    if (fileSize <= 0) {
        return;
    }

    std::ifstream in(g_materialPath.c_str(), std::ifstream::in);
    if (in.is_open()) {
        g_materialBuffer.reserve(static_cast<unsigned long>(fileSize));
        if (in.read(g_materialBuffer.data(), fileSize)) {
            g_material = Material::Builder()
                    .package((void*) g_materialBuffer.data(), (size_t) fileSize)
                    .build(*engine);
            g_materialInstance = g_material->createInstance();
        }
    }
}

static void readParameters() {
    std::ifstream in(g_paramsPath.c_str(), std::ifstream::in);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream lineStream(line);
            Param param;
            lineStream >> param.name;
            lineStream >> param.start;
            lineStream >> param.end;
            g_parameters.push_back(param);
        }
    }
}

static void setup(Engine* engine, View* view, Scene* scene) {
    g_meshSet = std::make_unique<MeshAssimp>(*engine);

    readMaterial(engine);
    readParameters();

    if (!g_materialInstance) {
        std::cerr << "The source material " << g_materialPath << " is invalid." << std::endl;
        return;
    }

    for (auto& filename : g_filenames) {
        g_meshSet->addFromFile(filename, g_meshMaterialInstances);
    }

    auto& tcm = engine->getTransformManager();
    auto ei = tcm.getInstance(g_meshSet->getRenderables()[0]);
    tcm.setTransform(ei, mat4f{ mat3f(g_config.scale), float3(0.0f, 0.0f, -4.0f) } *
            tcm.getWorldTransform(ei));

    auto& rcm = engine->getRenderableManager();
    for (auto renderable : g_meshSet->getRenderables()) {
        auto instance = rcm.getInstance(renderable);
        if (!instance) continue;

        rcm.setCastShadows(instance, true);

        for (size_t i = 0; i < rcm.getPrimitiveCount(instance); i++) {
            rcm.setMaterialInstanceAt(instance, i, g_materialInstance);
        }

        scene->addEntity(renderable);
    }

    g_light = EntityManager::get().create();
    LightManager::Builder(LightManager::Type::SUN)
            .color(Color::toLinear<ACCURATE>(sRGBColor{0.98f, 0.92f, 0.89f}))
            .intensity(110000.0f)
            .direction({0.6f, -1.0f, -0.8f})
            //.castShadows(true)
            .build(*engine, g_light);

    if (g_lightOn) {
        scene->addEntity(g_light);
    }

    for (const auto& p : g_parameters) {
        g_materialInstance->setParameter(p.name.c_str(), p.start);
    }

    if (!g_skyboxOn) {
        auto ibl = FilamentApp::get().getIBL();
        if (ibl) ibl->getSkybox()->setLayerMask(0xff, 0x00);
    } else {
        g_skybox = Skybox::Builder().color({
                ((g_clearColor >> 16) & 0xFF) / 255.0f,
                ((g_clearColor >>  8) & 0xFF) / 255.0f,
                ((g_clearColor      ) & 0xFF) / 255.0f,
                1.0f
        }).build(*engine);
        scene->setSkybox(g_skybox);
    }
}

template<typename T>
static LinearImage toLinear(size_t w, size_t h, size_t bpr, const uint8_t* src) {
    LinearImage result(w, h, 3);
    filament::math::float3* d = reinterpret_cast<filament::math::float3*>(result.getPixelRef(0, 0));
    for (size_t y = 0; y < h; ++y) {
        T const* p = reinterpret_cast<T const*>(src + y * bpr);
        for (size_t x = 0; x < w; ++x, p += 3) {
            filament::math::float3 sRGB(p[0], p[1], p[2]);
            sRGB /= std::numeric_limits<T>::max();
            *d++ = sRGBToLinear(sRGB);
        }
    }
    return result;
}

static void render(Engine*, View*, Scene*, Renderer*) {
    int frame = g_currentFrame - FRAME_TO_SKIP - 1;
    if (frame >= 0 && frame < g_materialVariantCount) {
        for (const auto& p : g_parameters) {
            g_materialInstance->setParameter(p.name.c_str(),
                    p.start + frame * ((p.end - p.start) / float(g_materialVariantCount - 1)));
        }
    }
}

static void postRender(Engine*, View* view, Scene*, Renderer* renderer) {
    int frame = g_currentFrame - FRAME_TO_SKIP - 1;
    // Account for the back buffer
    if (frame >= 1 && frame < g_materialVariantCount + 1) {
        frame -= 1;

        const Viewport& vp = view->getViewport();
        uint8_t* pixels = new uint8_t[vp.width * vp.height * 3];

        struct CaptureState {
            View* view = nullptr;
            int currentFrame = 0;
        };

        backend::PixelBufferDescriptor buffer(pixels, vp.width * vp.height * 3,
                backend::PixelBufferDescriptor::PixelDataFormat::RGB,
                backend::PixelBufferDescriptor::PixelDataType::UBYTE,
                [](void* buffer, size_t size, void* user) {
                    CaptureState* state = static_cast<CaptureState*>(user);
                    const Viewport& v = state->view->getViewport();

                    LinearImage image(toLinear<uint8_t>(v.width, v.height, v.width * 3,
                            static_cast<uint8_t*>(buffer)));

                    int digits = (int) log10 ((double) g_materialVariantCount) + 1;

                    std::ostringstream stringStream;
                    stringStream << "./" << g_prefix;
                    stringStream << std::setfill('0') << std::setw(digits);
                    stringStream << std::to_string(state->currentFrame);
                    stringStream << ".png";

                    std::string name = stringStream.str();
                    Path out(name);

                    std::ofstream outputStream(out, std::ios::binary | std::ios::trunc);
                    ImageEncoder::encode(outputStream, ImageEncoder::Format::PNG, image, "", name);

                    delete[] static_cast<uint8_t*>(buffer);
                    delete state;

                    g_savedFrames++;
                },
                new CaptureState { view, frame }
        );

        renderer->readPixels(
                (uint32_t) vp.left, (uint32_t) vp.bottom, vp.width, vp.height, std::move(buffer));
    }

    if (g_savedFrames.load() == g_materialVariantCount) {
        FilamentApp::get().close();
    }

    g_currentFrame++;
}

int main(int argc, char* argv[]) {
    int option_index = handleCommandLineArgments(argc, argv, &g_config);
    int num_args = argc - option_index;
    if (num_args < 1 || g_materialPath.isEmpty() || g_paramsPath.isEmpty()) {
        printUsage(argv[0]);
        return 1;
    }

    for (int i = option_index; i < argc; i++) {
        utils::Path filename = argv[i];
        if (!filename.exists()) {
            std::cerr << "file " << argv[i] << " not found!" << std::endl;
            return 1;
        }
        g_filenames.push_back(filename);
    }

    g_config.title = "Frame Generator";
    g_config.headless = true;
    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.run(g_config,
            setup, cleanup, FilamentApp::ImGuiCallback(), render, postRender, 512, 512);

    return 0;
}
