/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Processing/PixelFormatInfo.h>
#include <Processing/ImageToProcess.h>
#include <Processing/ImageConvert.h>
#include <Processing/ImageAssetProducer.h>
#include <Processing/ImageFlags.h>
#include <Converters/FIR-Weights.h>
#include <Converters/Cubemap.h>
#include <Converters/PixelOperation.h>
#include <Converters/Histogram.h>
#include <ImageLoader/ImageLoaders.h>
#include <BuilderSettings/BuilderSettingManager.h>
#include <BuilderSettings/PresetSettings.h>

#include <AzFramework/StringFunc/StringFunc.h>

#include <AzToolsFramework/API/EditorAssetSystemAPI.h>

// for texture splitting
// minimum number of low level mips will be saved in the base file.
#define MinPersistantMips 3
// minimum texture size to be splitted. A texture will only be split when the size is larger than this number
#define MinSizeToSplit 1 << 5

#if defined(AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS)
#if defined(TOOLS_SUPPORT_JASPER)
    #include AZ_RESTRICTED_FILE_EXPLICIT(ImageProcess, Jasper)
#endif
#if defined(TOOLS_SUPPORT_PROVO)
    #include AZ_RESTRICTED_FILE_EXPLICIT(ImageProcess, Provo)
#endif
#if defined(TOOLS_SUPPORT_SALEM)
    #include AZ_RESTRICTED_FILE_EXPLICIT(ImageProcess, Salem)
#endif
#endif

namespace ImageProcessingAtom
{
    enum ConvertStep
    {
        StepValidateInput = 0,
        StepGenerateColorChart,
        StepConvertToLinear,
        StepSwizzle,
        StepCubemapLayout,
        StepPreNormalize,
        StepGenerateIBL,
        StepMipmap,
        StepGlossFromNormal,
        StepPostNormalize,
        StepCreateHighPass,
        StepConvertOutputColorSpace,
        StepAlphaImage,
        StepConvertPixelFormat,
        StepSaveToFile,
        StepAll
    };

    [[maybe_unused]] const char ProcessStepNames[StepAll][64] =
    {
        "ValidateInput",
        "GenerateColorChart",
        "ConvertToLinear",
        "Swizzle",
        "CubemapLayout",
        "PreNormalize",
        "GenerateIBL",
        "Mipmap",
        "GlossFromNormal",
        "PostNormalize",
        "CreateHighPass",
        "ConvertOutputColorSpace",
        "AlphaImage",
        "ConvertPixelFormat",
        "SaveToFile",
    };

    const char* SpecularCubemapSuffix = "_iblspecular";
    const char* DiffuseCubemapSuffix = "_ibldiffuse";

    IImageObjectPtr ImageConvertProcess::GetOutputImage()
    {
        if (m_image)
        {
            return m_image->Get();
        }
        return nullptr;
    }

    IImageObjectPtr ImageConvertProcess::GetOutputAlphaImage()
    {
        return m_alphaImage;
    }

    IImageObjectPtr ImageConvertProcess::GetOutputIBLSpecularCubemap()
    {
        return m_iblSpecularCubemapImage;
    }

    IImageObjectPtr ImageConvertProcess::GetOutputIBLDiffuseCubemap()
    {
        return m_iblDiffuseCubemapImage;
    }

    void ImageConvertProcess::GetAppendOutputProducts(AZStd::vector<AssetBuilderSDK::JobProduct>& outProducts)
    {
        for (const auto& path : m_jobProducts)
        {
            outProducts.push_back(path);
        }
    }

    const ImageConvertProcessDescriptor* ImageConvertProcess::GetInputDesc() const
    {
        return m_input.get();
    }

    ImageConvertProcess::ImageConvertProcess(AZStd::unique_ptr<ImageConvertProcessDescriptor>&& descriptor)
        : m_image(nullptr)
        , m_progressStep(0)
        , m_isFinished(false)
        , m_isSucceed(false)
        , m_processTime(0)
    {
        m_input = AZStd::move(descriptor);
    }

    ImageConvertProcess::~ImageConvertProcess()
    {
        delete m_image;
    }

    bool ImageConvertProcess::IsConvertToCubemap()
    {
        return m_input->m_presetSetting.m_cubemapSetting != nullptr;
    }

    bool ImageConvertProcess::IsPreconvolvedCubemap()
    {
        AZStd::unique_ptr<CubemapSettings>& cubemapSettings = m_input->m_presetSetting.m_cubemapSetting;
        return (cubemapSettings != nullptr && cubemapSettings->m_requiresConvolve == false);
    }

    void ImageConvertProcess::UpdateProcess()
    {
        if (m_isFinished)
        {
            return;
        }

        auto stepStartTime = AZStd::GetTimeUTCMilliSecond();

        switch (m_progressStep)
        {
        case StepValidateInput:
            // validate
            if (!ValidateInput())
            {
                m_isSucceed = false;
                break;
            }

            // set start time
            m_startTime = AZStd::GetTimeUTCMilliSecond();

            // identify the alpha content of input image if gloss from normal wasn't set
            m_alphaContent = m_input->m_inputImage->GetAlphaContent();

            // Create image for process.
            // If this is not a pre-convolved cubemap we only copy the highest mip until we figure out what to do with input's mipmaps.
            {
                uint32 mipsToClone = IsPreconvolvedCubemap() ? (std::numeric_limits<uint32>::max)() : 1;
                m_image = new ImageToProcess(IImageObjectPtr(m_input->m_inputImage->Clone(mipsToClone)));
            }

            break;
        case StepGenerateIBL:
            if (IsConvertToCubemap())
            {
                // check and generate IBL specular and diffuse, if necessary
                AZStd::unique_ptr<CubemapSettings>& cubemapSettings = m_input->m_presetSetting.m_cubemapSetting;
                if (cubemapSettings->m_generateIBLSpecular && !cubemapSettings->m_iblSpecularPreset.IsNull())
                {
                    CreateIBLCubemap(cubemapSettings->m_iblSpecularPreset, SpecularCubemapSuffix, m_iblSpecularCubemapImage);
                }

                if (cubemapSettings->m_generateIBLDiffuse && !cubemapSettings->m_iblDiffusePreset.IsNull())
                {
                    CreateIBLCubemap(cubemapSettings->m_iblDiffusePreset, DiffuseCubemapSuffix, m_iblDiffuseCubemapImage);
                }
            }

            if (m_input->m_presetSetting.m_generateIBLOnly)
            {
                // this preset doesn't output an image of its own, just the IBL cubemaps
                m_isSucceed = true;
                m_isFinished = true;
            }
            break;
        case StepGenerateColorChart:
            // GenerateColorChart.
            if (m_input->m_presetSetting.m_isColorChart)
            {
                // Convert to uncompressed format if it's compressed format. For example, loaded from DDS file.
                if (!CPixelFormats::GetInstance().IsPixelFormatUncompressed(m_image->Get()->GetPixelFormat()))
                {
                    m_image->ConvertFormat(ePixelFormat_R32G32B32A32F);
                }

                m_image->CreateColorChart();
            }
            break;
        case StepConvertToLinear:
            // convert to linear space and the output image pixel format should be rgba32f
            ConvertToLinear();
            break;
        case StepSwizzle:
            // convert texture format.
            if (m_input->m_presetSetting.m_swizzle.size() >= 4)
            {
                m_image->Get()->Swizzle(m_input->m_presetSetting.m_swizzle.substr(0, 4).c_str());
                m_alphaContent = m_image->Get()->GetAlphaContent();
            }

            // convert gloss map (alhpa channel) from legacy distribution to new one
            if (m_input->m_presetSetting.m_isLegacyGloss)
            {
                m_image->Get()->ConvertLegacyGloss();
            }
            break;
        case StepCubemapLayout:
            // convert cubemap image's layout to vertical strip used in game.
            if (IsConvertToCubemap())
            {
                if (!m_image->ConvertCubemapLayout(CubemapLayoutVertical))
                {
                    m_image->Set(nullptr);
                }
            }
            break;
        case StepPreNormalize:
            // normalize base image before mipmap generation if glossfromnormals is enabled and require normalize
            if (m_input->m_presetSetting.m_isMipRenormalize && m_input->m_presetSetting.m_glossFromNormals)
            {
                // Normalize the base mip map. This has to be done explicitly because we need to disable mip renormalization to
                // preserve the normal length when deriving the normal variance
                m_image->Get()->NormalizeVectors(0, 1);
            }
            break;
        case StepMipmap:
            // generate mipmaps
            if (IsConvertToCubemap())
            {
                if (m_input->m_presetSetting.m_cubemapSetting->m_requiresConvolve)
                {
                    FillCubemapMipmaps();
                }
            }
            else
            {
                FillMipmaps();
            }
            // add image flag
            if (m_input->m_presetSetting.m_suppressEngineReduce || m_input->m_textureSetting.m_suppressEngineReduce)
            {
                m_image->Get()->AddImageFlags(EIF_SupressEngineReduce);
            }
            break;
        case StepGlossFromNormal:
            // get gloss from normal for all mipmaps and save to alpha channel
            if (m_input->m_presetSetting.m_glossFromNormals)
            {
                bool hasAlpha = (m_alphaContent == EAlphaContent::eAlphaContent_OnlyBlack
                    || m_alphaContent == EAlphaContent::eAlphaContent_OnlyBlackAndWhite
                    || m_alphaContent == EAlphaContent::eAlphaContent_Greyscale);

                m_image->Get()->GlossFromNormals(hasAlpha);
                // set alpha content so it won't be ignored later.
                m_alphaContent = EAlphaContent::eAlphaContent_Greyscale;
            }
            break;
        case StepPostNormalize:
            // normalize all the other mipmaps
            if (!IsConvertToCubemap() && m_input->m_presetSetting.m_isMipRenormalize)
            {
                if (m_input->m_presetSetting.m_glossFromNormals)
                {
                    // normalize other mips except first mip
                    m_image->Get()->NormalizeVectors(1, 100);
                }
                else
                {
                    // normalize all mips
                    m_image->Get()->NormalizeVectors(0, 100);
                }

                m_image->Get()->AddImageFlags(EIF_RenormalizedTexture);
            }
            break;
        case StepCreateHighPass:
            if (m_input->m_presetSetting.m_highPassMip > 0)
            {
                m_image->CreateHighPass(m_input->m_presetSetting.m_highPassMip);
            }
            break;
        case StepConvertOutputColorSpace:
            // convert image from linear space to desired output color space
            ConvertToOuputColorSpace();
            break;
        case StepAlphaImage:
            // save alpha channel to separate image if it's needed
            CreateAlphaImage();
            break;
        case StepConvertPixelFormat:
            // convert pixel format
            ConvertPixelformat();
            break;
        case StepSaveToFile:
            // save to file
            if (!m_input->m_isPreview)
            {
                m_isSucceed = SaveOutput();
            }
            else
            {
                m_isSucceed = true;
            }
            break;
        }

        auto stepEndTime = AZStd::GetTimeUTCMilliSecond();

        if (stepEndTime - stepStartTime > 1000)
        {
            AZ_TracePrintf("Image Processing", "Step [%s] took %f seconds\n", ProcessStepNames[m_progressStep],
                (stepEndTime - stepStartTime) / 1000.0);
        }

        m_progressStep++;

        if (m_image == nullptr || m_image->Get() == nullptr || m_progressStep >= StepAll)
        {
            m_isFinished = true;
            AZStd::sys_time_t endTime = AZStd::GetTimeUTCMilliSecond();
            m_processTime = static_cast<double>(endTime - m_startTime) / 1000.0;
        }

        // output conversion log
        if (m_isSucceed && m_isFinished)
        {
            [[maybe_unused]] const uint32 sizeTotal = m_image->Get()->GetTextureMemory();
            if (m_input->m_isPreview)
            {
                AZ_TracePrintf("Image Processing", "Image (%d bytes) converted in %f seconds\n", sizeTotal, m_processTime);
            }
            else if (m_input->m_presetSetting.m_generateIBLOnly)
            {
                AZ_TracePrintf("Image Processing", "Image (IBL Only) processed in %f seconds\n", m_processTime);
            }
            else
            {
                AZ_TracePrintf("Image Processing", "Image converted with preset [%s] [%s] and saved to [%s] (%d bytes) taking %f seconds\n",
                    m_input->m_presetSetting.m_name.c_str(),
                    m_input->m_filePath.c_str(),
                    m_input->m_outputFolder.c_str(), sizeTotal, m_processTime);
            }
        }
    }

    void ImageConvertProcess::ProcessAll()
    {
        while (!m_isFinished)
        {
            UpdateProcess();
        }
    }

    float ImageConvertProcess::GetProgress()
    {
        return m_progressStep / (float)StepAll;
    }

    bool ImageConvertProcess::IsFinished()
    {
        return m_isFinished;
    }

    bool ImageConvertProcess::IsSucceed()
    {
        return m_isSucceed;
    }

    // function to get desired output image extent
    void GetOutputExtent(AZ::u32 inputWidth, AZ::u32 inputHeight, AZ::u32& outWidth, AZ::u32& outHeight, AZ::u32& outReduce,
        const TextureSettings* textureSettings, const PresetSettings* presetSettings)
    {
        AZ_Assert(&outWidth != &outHeight, "outWidth and outHeight shouldn't use same address");

        outWidth = inputWidth;
        outHeight = inputHeight;

        if (textureSettings == nullptr || presetSettings == nullptr)
        {
            return;
        }

        // don't do any reduce for color chart
        if (presetSettings->m_isColorChart)
        {
            return;
        }

        // get suitable size for dest pixel format
        CPixelFormats::GetInstance().GetSuitableImageSize(presetSettings->m_pixelFormat, inputWidth, inputHeight,
            outWidth, outHeight);


        // desired reduce level. 1 means reduce one level
        uint sizeReduceLevel = textureSettings->m_sizeReduceLevel;

        outReduce = 0;

        // reduce to not exceed max texture size
        if (presetSettings->m_maxTextureSize > 0)
        {
            while (outWidth > presetSettings->m_maxTextureSize || outHeight > presetSettings->m_maxTextureSize)
            {
                outWidth >>= 1;
                outHeight >>= 1;
                outReduce++;
            }
        }

        // if it requires to reduce more and the result size will still larger than min texture size, then reduce
        while (outReduce < sizeReduceLevel &&
            (outWidth >= presetSettings->m_minTextureSize * 2 && outHeight >= presetSettings->m_minTextureSize * 2))
        {
            outWidth >>= 1;
            outHeight >>= 1;
            outReduce++;
        }
    }

    bool ImageConvertProcess::ConvertToLinear()
    {
        // de-gamma only if the input is sRGB. this will convert other uncompressed format to RGBA32F
        return m_image->GammaToLinearRGBA32F(m_input->m_presetSetting.m_srcColorSpace == ColorSpace::sRGB);
    }

    // mipmap generation
    bool ImageConvertProcess::FillMipmaps()
    {
        //this function only works with pixel format rgba32f
        const EPixelFormat srcPixelFormat = m_image->Get()->GetPixelFormat();
        if (srcPixelFormat != ePixelFormat_R32G32B32A32F)
        {
            AZ_Assert(false, "%s only works with pixel format rgba32f", __FUNCTION__);
            return false;
        }

        // only if the src image has one mip
        if (m_image->Get()->GetMipCount() != 1)
        {
            AZ_Assert(false, "%s called for a mipmapped image. ", __FUNCTION__);
            return false;
        }

        // get output image size
        uint32 outWidth;
        uint32 outHeight;
        uint32 outReduce = 0;
        GetOutputExtent(m_image->Get()->GetWidth(0), m_image->Get()->GetHeight(0), outWidth, outHeight, outReduce, &m_input->m_textureSetting,
            &m_input->m_presetSetting);

        // max mipmap count
        uint32 mipCount = UINT32_MAX;
        if (m_input->m_presetSetting.m_mipmapSetting == nullptr || !m_input->m_textureSetting.m_enableMipmap)
        {
            mipCount = 1;
        }

        // create new new output image with proper side
        IImageObjectPtr outImage(IImageObject::CreateImage(outWidth, outHeight, mipCount, ePixelFormat_R32G32B32A32F));

        // filter setting for mip map generation
        float blurH = 0;
        float blurV = 0;

        // fill mipmap data for uncompressed output image
        for (uint32 mip = 0; mip < outImage->GetMipCount(); mip++)
        {
            FilterImage(m_input->m_textureSetting.m_mipGenType, m_input->m_textureSetting.m_mipGenEval, blurH, blurV, m_image->Get(), 0, outImage, mip, nullptr, nullptr);
        }

        // transfer alpha coverage
        if (m_input->m_textureSetting.m_maintainAlphaCoverage)
        {
            outImage->TransferAlphaCoverage(&m_input->m_textureSetting, m_image->Get());
        }

        // set back to image
        m_image->Set(outImage);
        return true;
    }

    void ImageConvertProcess::CreateAlphaImage()
    {
        // if alpha content doesn't have alpha or we need to discard alpha, skip
        // we won't create alpha image for cubemap too
        if (m_alphaContent == EAlphaContent::eAlphaContent_Absent
            || m_alphaContent == EAlphaContent::eAlphaContent_OnlyWhite
            || m_input->m_presetSetting.m_discardAlpha || IsConvertToCubemap())
        {
            return;
        }

        // if dest format could save alpha, skip too
        if (!CPixelFormats::GetInstance().IsPixelFormatWithoutAlpha(m_input->m_presetSetting.m_pixelFormat))
        {
            return;
        }

        // now create alpha image
        ImageToProcess alphaImage(m_image->Get());
        alphaImage.ConvertFormat(ePixelFormat_A8);

        // validate pixelformatalpha
        if (CPixelFormats::GetInstance().IsFormatSingleChannel(m_input->m_presetSetting.m_pixelFormatAlpha))
        {
            alphaImage.ConvertFormat(m_input->m_presetSetting.m_pixelFormatAlpha);
        }
        else
        {
            //For ASTC compression we need to clear out the alpha to get accurate rgb compression.
            if (IsASTCFormat(m_input->m_presetSetting.m_pixelFormat))
            {
                alphaImage.ConvertFormat(ePixelFormat_R8G8B8X8);
                alphaImage.ConvertFormat(m_input->m_presetSetting.m_pixelFormatAlpha);
            }
            else
            {
                AZ_Assert(false, "PixelFormatAlpha only supports single channel pixel formats or ASTC formats");
            }
        }

        // get final result and save it to member variable for later use
        m_alphaImage = alphaImage.Get();

        m_image->Get()->AddImageFlags(EIF_AttachedAlpha);
    }

    // pixel format conversion
    bool ImageConvertProcess::ConvertPixelformat()
    {
        //set up compress option
        ICompressor::EQuality quality;
        if (m_input->m_isPreview)
        {
            quality = ICompressor::eQuality_Preview;
        }
        else
        {
            quality = ICompressor::eQuality_Normal;
        }

        // set the compression options
        m_image->GetCompressOption().compressQuality = quality;
        m_image->GetCompressOption().rgbWeight = m_input->m_presetSetting.GetColorWeight();
        m_image->GetCompressOption().discardAlpha = m_input->m_presetSetting.m_discardAlpha;

        //For ASTC compression we need to clear out the alpha to get accurate rgb compression.
        if(m_alphaImage && IsASTCFormat(m_input->m_presetSetting.m_pixelFormat))
        {
            m_image->GetCompressOption().discardAlpha = true;
        }

        m_image->ConvertFormat(m_input->m_presetSetting.m_pixelFormat);

        return true;
    }

    // convert color space from linear to sRGB space if it's necessary
    bool ImageConvertProcess::ConvertToOuputColorSpace()
    {
        if (m_input->m_presetSetting.m_destColorSpace == ColorSpace::sRGB)
        {
            m_image->LinearToGamma();
        }
        else if (m_input->m_presetSetting.m_destColorSpace == ColorSpace::autoSelect)
        {
            // check the compressor's colorspace preference
            const EPixelFormat sourceFormat = m_image->Get()->GetPixelFormat();
            const EPixelFormat destinationFormat = m_input->m_presetSetting.m_pixelFormat;

            const bool isSourceFormatUncompressed = CPixelFormats::GetInstance().IsPixelFormatUncompressed(sourceFormat);
            const bool isDestinationFormatUncompressed = CPixelFormats::GetInstance().IsPixelFormatUncompressed(destinationFormat);

            // compression is only required if either the source or destination is uncompressed
            if (isSourceFormatUncompressed != isDestinationFormatUncompressed)
            {
                // find out if the process is compressing or decompressing
                const bool isCompressing = isSourceFormatUncompressed ? true : false;
                const EPixelFormat outputFormat = isCompressing ? destinationFormat : sourceFormat;

                ICompressorPtr compressor = ICompressor::FindCompressor(outputFormat, m_input->m_presetSetting.m_destColorSpace, isCompressing);

                // find out if the compressor has a preference to any specific colorspace
                const ColorSpace compressorColorSpace = compressor->GetSupportedColorSpace(outputFormat);
                if (compressorColorSpace == ColorSpace::sRGB)
                {
                    m_image->LinearToGamma();
                    return true;
                }
                else if (compressorColorSpace == ColorSpace::linear)
                {
                    return true;
                }
            }

            // convert to sRGB color space if it's dark image (converting bright images decreases image quality)
            bool bThresholded = false;
            {
                Histogram<256> histogram;
                if (ComputeLuminanceHistogram(m_image->Get(), histogram))
                {
                    const size_t medianBinIndex = 116;
                    float percentage = histogram.getPercentage(medianBinIndex, 255);

                    // The image has significant amount of dark pixels, it's good to use sRGB
                    bThresholded = (percentage < 50.0f);
                }
            }

            if (bThresholded)
            {
                bool convertToSRGB = true;

                // if the image is BC1 compressible, additionally estimate the conversion error
                // to only convert if it doesn't introduce error
                if (CPixelFormats::GetInstance().IsImageSizeValid(ePixelFormat_BC1, m_image->Get()->GetWidth(0),
                    m_image->Get()->GetHeight(0), false))
                {
                    //get image in RGB space
                    ImageToProcess imageProcess(m_image->Get());
                    imageProcess.LinearToGamma();

                    ICompressor::CompressOption option;
                    option.compressQuality = ICompressor::eQuality_Preview;
                    option.rgbWeight = m_input->m_presetSetting.GetColorWeight();

                    float errorLinearBC1;
                    float errorSrgbBC1;
                    GetBC1CompressionErrors(m_image->Get(), errorLinearBC1, errorSrgbBC1, option);

                    // Don't convert if it would lower the image quality when saved as sRGB according to GetDXT1GammaCompressionError()
                    if (errorSrgbBC1 >= errorLinearBC1)
                    {
                        convertToSRGB = false;
                    }
                }

                // our final conclusion: if the texture had a significant percentage of dark pixels and,
                // if applicable, it was BC1 compressible and gamma compression wouldn't introduce error,
                // then we convert it to sRGB
                if (convertToSRGB)
                {
                    m_image->LinearToGamma();
                }
            }
        }
        return true;
    }

    bool ImageConvertProcess::ValidateInput()
    {
        // validate the input image and output settings here.
        uint32 dwWidth, dwHeight;
        dwWidth = m_input->m_inputImage->GetWidth(0);
        dwHeight = m_input->m_inputImage->GetHeight(0);

        EPixelFormat dstFmt = m_input->m_presetSetting.m_pixelFormat;

        // check if whether input image can be a cubemap
        if (m_input->m_presetSetting.m_cubemapSetting)
        {
            // check requirements for pre-convolved cubemaps
            // note: only check formatting if there are multiple mip levels in the source cubemap,
            // since some of the conversion functions should not be used when mips are present
            if (IsPreconvolvedCubemap() && m_input->m_inputImage->GetMipCount() > 1)
            {
                if (m_input->m_presetSetting.m_srcColorSpace != ColorSpace::linear)
                {
                    AZ_Error("Image Processing", false, "Pre-convolved environment map image must use linear colorspace");
                    return false;
                }

                if (m_input->m_inputImage->GetPixelFormat() != ePixelFormat_R32G32B32A32F
                    && m_input->m_inputImage->GetPixelFormat() != ePixelFormat_R16G16B16A16F)
                {
                    AZ_Error("Image Processing", false, "Pre-convolved environment map image must be R32G32B32A32F or R16G16B16A16F");
                    return false;
                }

                CubemapLayoutInfo* layoutInfo = CubemapLayout::GetCubemapLayoutInfo(m_input->m_inputImage);
                if (IsValidLatLongMap(m_input->m_inputImage) || layoutInfo->m_type != CubemapLayoutVertical)
                {
                    AZ_Error("Image Processing", false, "Pre-convolved environment map image with multiple mips must be in Vertical layout format");
                    return false;
                }
            }
            else if (CubemapLayout::GetCubemapLayoutInfo(m_input->m_inputImage) == nullptr && !IsValidLatLongMap(m_input->m_inputImage))
            {
                AZ_Error("Image Processing", false, "Environment map image size %dx%d is invalid. Requires power of two with 6x1, 1x6, 4x3 or 3x4 layouts"
                    " or 2x1 latitude-longitude map", dwWidth, dwHeight);
                return false;
            }
        }
        else if (!CPixelFormats::GetInstance().IsImageSizeValid(dstFmt, dwWidth, dwHeight, false))
        {
            AZ_Warning("Image Processing", false, "Image size will be scaled for pixel format %s", CPixelFormats::GetInstance().GetPixelFormatInfo(dstFmt)->szName);
        }

#if defined(AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS)
#define AZ_RESTRICTED_PLATFORM_EXPANSION(CodeName, CODENAME, codename, PrivateName, PRIVATENAME, privatename, PublicName, PUBLICNAME, publicname, PublicAuxName1, PublicAuxName2, PublicAuxName3) \
    if (ImageProcess##PrivateName::DoesSupport(m_input->m_platform))                                                                                                                              \
    {                                                                                                                                                                                             \
        if (!ImageProcess##PrivateName::IsPixelFormatSupported(m_input->m_presetSetting.m_pixelFormat))                                                                                           \
        {                                                                                                                                                                                         \
            AZ_Error("Image Processing", false, "Unsupported pixel format %s for %s",                                                                                                             \
            CPixelFormats::GetInstance().GetPixelFormatInfo(dstFmt)->szName, m_input->m_platform.c_str());                                                                                        \
            return false;                                                                                                                                                                         \
        }                                                                                                                                                                                         \
    }
        AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS
#undef AZ_RESTRICTED_PLATFORM_EXPANSION
#endif //AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS

            return true;
    }

    bool ImageConvertProcess::SaveOutput()
    {
        // if the folder wasn't specified, skip
        if (m_input->m_outputFolder.empty())
        {
            AZ_Error("Image Processing", false, "No output folder provided for saving");
            return false;
        }

        // [GFX TODO] [ATOM-781] Platform related image prepare need to be reworked on.
        // Disabled for now since it's not working properly for atom
#if IMAGEBUILDER_ENABLE_PLATFORM_EXPORT_PREPARE
#if defined(AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS)
#define AZ_RESTRICTED_PLATFORM_EXPANSION(CodeName, CODENAME, codename, PrivateName, PRIVATENAME, privatename, PublicName, PUBLICNAME, publicname, PublicAuxName1, PublicAuxName2, PublicAuxName3) \
    if (ImageProcess##PrivateName::DoesSupport(m_input->m_platform))                                                                                                                              \
    {                                                                                                                                                                                             \
        ImageProcess##PrivateName::PrepareImageForExport(m_image->Get());                                                                                                                         \
        ImageProcess##PrivateName::PrepareImageForExport(m_alphaImage);                                                                                                                           \
    }
        AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS
#undef AZ_RESTRICTED_PLATFORM_EXPANSION
#endif //AZ_TOOLS_EXPAND_FOR_RESTRICTED_PLATFORMS
#endif

        // cubemaps can have a specific subId, standard images use the subId specified in StreamingImageAsset
        uint32_t subId = IsConvertToCubemap() ? m_input->m_presetSetting.m_cubemapSetting->m_subId : RPI::StreamingImageAsset::GetImageAssetSubId();

        // Save the image to atom image assets
        ImageAssetProducer assetProducer(
            m_image->Get(),
            m_input->m_outputFolder,
            m_input->m_sourceAssetId,
            m_input->m_imageName,
            m_input->m_presetSetting.m_numResidentMips,
            subId);

        if (assetProducer.BuildImageAssets())
        {
            m_jobProducts = assetProducer.GetJobProducts();
            return true;
        }

        AZ_Error("Image Processing", false, "Failed to generate StreamingImageAsset");
        return false;
    }

    ImageConvertProcess* CreateImageConvertProcess(const AZStd::string& imageFilePath, const AZStd::string& exportDir
        , const PlatformName& platformName, AZStd::vector<AssetBuilderSDK::JobProduct>& jobProducts, AZ::SerializeContext* context)
    {
        AZStd::unique_ptr<ImageConvertProcessDescriptor> desc = AZStd::make_unique<ImageConvertProcessDescriptor>();

        TextureSettings& textureSettings = desc->m_textureSetting;
        MultiplatformTextureSettings multiTextureSetting;
        bool canOverridePreset = false;

        multiTextureSetting = TextureSettings::GetMultiplatformTextureSetting(imageFilePath, canOverridePreset, context);
        if (multiTextureSetting.size() == 0)
        {
            AZ_Error("Image Processing", false, "Failed to generate texture setting");
            return nullptr;
        }

        if (multiTextureSetting.find(platformName) != multiTextureSetting.end())
        {
            textureSettings = multiTextureSetting[platformName];
        }
        else
        {
            PlatformName defaultPlatform = BuilderSettingManager::s_defaultPlatform;
            if (multiTextureSetting.find(defaultPlatform) != multiTextureSetting.end())
            {
                textureSettings = multiTextureSetting[defaultPlatform];
            }
            else
            {
                textureSettings = (*multiTextureSetting.begin()).second;
            }
        }

        // Load image. Do it earlier so GetSuggestedPreset function could use the information of file to choose better preset
        IImageObjectPtr srcImage(LoadImageFromFile(imageFilePath));
        if (srcImage == nullptr)
        {
            AZ_Error("Image Processing", false, "Load image file %s failed", imageFilePath.c_str());
            return nullptr;
        }

        // if get textureSetting failed, use the default texture setting, and find suitable preset for this file
        // in very rare user case, an old texture setting file may not have a preset. We fix it over here too. 
        if (textureSettings.m_preset.IsNull())
        {
            textureSettings.m_preset = BuilderSettingManager::Instance()->GetSuggestedPreset(imageFilePath, srcImage);
        }

        // Get preset
        AZStd::string_view filePath;
        const PresetSettings* preset = BuilderSettingManager::Instance()->GetPreset(textureSettings.m_preset, platformName, &filePath);

        if (preset == nullptr)
        {
            AZStd::string uuidStr;
            textureSettings.m_preset.ToString(uuidStr);
            AZ_Assert(false, "%s cannot find image preset with ID %s.", imageFilePath.c_str(), uuidStr.c_str());
            return nullptr;
        }

        desc->m_presetSetting = *preset;
        desc->m_platform = platformName;
        desc->m_filePath = filePath;
        desc->m_inputImage = srcImage;
        desc->m_isPreview = false;
        desc->m_isStreaming = BuilderSettingManager::Instance()->GetBuilderSetting(platformName)->m_enableStreaming;
        desc->m_outputFolder = exportDir;
        desc->m_jobProducts = &jobProducts;
        AzFramework::StringFunc::Path::GetFullFileName(imageFilePath.c_str(), desc->m_imageName);

        // Get source asset id. Create random id if it's not found which is useful if this functions wasn't called under asset builder environment. For example, unit test.
        AZStd::string watchFolder;
        AZ::Data::AssetInfo catalogAssetInfo;
        bool sourceInfoFound = false;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(sourceInfoFound, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
            imageFilePath.c_str(), catalogAssetInfo, watchFolder);
        desc->m_sourceAssetId = sourceInfoFound ? catalogAssetInfo.m_assetId : AZ::Data::AssetId(AZ::Uuid::CreateRandom());

        // Create convert process
        ImageConvertProcess* process = new ImageConvertProcess(AZStd::move(desc));

        return process;
    }

    void ImageConvertProcess::CreateIBLCubemap(AZ::Uuid presetUUID, const char* fileNameSuffix, IImageObjectPtr& cubemapImage)
    {
        const AZStd::string& platformId = m_input->m_platform;
        AZStd::string_view filePath;
        const PresetSettings* presetSettings = BuilderSettingManager::Instance()->GetPreset(presetUUID, platformId, &filePath);
        if (presetSettings == nullptr)
        {
            AZ_Error("Image Processing", false, "Couldn't find preset for IBL cubemap generation");
            return;
        }

        // generate export file name
        AZStd::string fileName;
        AzFramework::StringFunc::Path::GetFileName(m_input->m_imageName.c_str(), fileName);
        fileName += fileNameSuffix;

        AZStd::string extension;
        AzFramework::StringFunc::Path::GetExtension(m_input->m_imageName.c_str(), extension);
        fileName += extension;

        AZStd::string outProductPath;
        AzFramework::StringFunc::Path::Join(m_input->m_outputFolder.c_str(), fileName.c_str(), outProductPath, true, true);

        // the diffuse irradiance cubemap is generated with a separate ImageConvertProcess
        TextureSettings textureSettings = m_input->m_textureSetting;
        textureSettings.m_preset = presetUUID;

        AZStd::unique_ptr<ImageConvertProcessDescriptor> desc = AZStd::make_unique<ImageConvertProcessDescriptor>();
        desc->m_presetSetting = *presetSettings;
        desc->m_textureSetting = textureSettings;
        desc->m_platform = platformId;
        desc->m_filePath = filePath;
        desc->m_inputImage = m_input->m_inputImage;
        desc->m_isPreview = false;
        desc->m_isStreaming = m_input->m_isStreaming;
        desc->m_outputFolder = m_input->m_outputFolder;
        desc->m_imageName = fileName;
        desc->m_sourceAssetId = m_input->m_sourceAssetId;

        AZStd::unique_ptr<ImageConvertProcess> imageConvertProcess = AZStd::make_unique<ImageConvertProcess>(AZStd::move(desc));
        if (!imageConvertProcess)
        {
            AZ_Error("Image Processing", false, "Failed to create image convert process for the IBL cubemap");
            return;
        }

        imageConvertProcess->ProcessAll();
        if (!imageConvertProcess->IsSucceed())
        {
            AZ_Error("Image Processing", false, "Image convert process for the IBL cubemap failed");
            return;
        }

        // append the output products to the job's product list
        imageConvertProcess->GetAppendOutputProducts(*m_input->m_jobProducts);

        // store the output cubemap so it can be accessed by unit tests
        cubemapImage = imageConvertProcess->m_image->Get();
    }

    bool ConvertImageFile(const AZStd::string& imageFilePath, const AZStd::string& exportDir,
        const PlatformName& platformName, AZ::SerializeContext* context, AZStd::vector<AssetBuilderSDK::JobProduct>& outProducts)
    {
        bool result = false;
        ImageConvertProcess* process = CreateImageConvertProcess(imageFilePath, exportDir, platformName, outProducts, context);
        if (process)
        {
            process->ProcessAll();
            result = process->IsSucceed();
            if (result)
            {
                process->GetAppendOutputProducts(outProducts);
            }
            delete process;
        }
        return result;
    }

    IImageObjectPtr MergeOutputImageForPreview(IImageObjectPtr image, IImageObjectPtr alphaImage)
    {
        if (!image)
        {
            return IImageObjectPtr();
        }

        ImageToProcess imageToProcess(image);
        imageToProcess.ConvertFormat(ePixelFormat_R8G8B8A8);
        IImageObjectPtr previewImage = imageToProcess.Get();

        // If there is separate Alpha image, combine it with output
        if (alphaImage)
        {
            // Create pixel operation function for rgb and alpha images
            IPixelOperationPtr imageOp = CreatePixelOperation(ePixelFormat_R8G8B8A8);
            IPixelOperationPtr alphaOp = CreatePixelOperation(ePixelFormat_A8);

            // Convert the alpha image to A8 first
            ImageToProcess imageToProcess2(alphaImage);
            imageToProcess2.ConvertFormat(ePixelFormat_A8);
            IImageObjectPtr previewImageAlpha = imageToProcess2.Get();

            const uint32 imageMips = previewImage->GetMipCount();
            [[maybe_unused]] const uint32 alphaMips = previewImageAlpha->GetMipCount();

            // Get count of bytes per pixel for both rgb and alpha images
            uint32 imagePixelBytes = CPixelFormats::GetInstance().GetPixelFormatInfo(ePixelFormat_R8G8B8A8)->bitsPerBlock / 8;
            uint32 alphaPixelBytes = CPixelFormats::GetInstance().GetPixelFormatInfo(ePixelFormat_A8)->bitsPerBlock / 8;

            AZ_Assert(imageMips <= alphaMips, "Mip level of alpha image is less than origin image!");

            // For each mip level, set the alpha value to the image
            for (uint32 mipLevel = 0; mipLevel < imageMips; ++mipLevel)
            {
                const uint32 pixelCount = previewImage->GetPixelCount(mipLevel);
                [[maybe_unused]] const uint32 alphaPixelCount = previewImageAlpha->GetPixelCount(mipLevel);

                AZ_Assert(pixelCount == alphaPixelCount, "Pixel count for image and alpha image at mip level %d is not equal!", mipLevel);

                uint8* imageBuf;
                uint32 pitch;
                previewImage->GetImagePointer(mipLevel, imageBuf, pitch);

                uint8* alphaBuf;
                uint32 alphaPitch;
                previewImageAlpha->GetImagePointer(mipLevel, alphaBuf, alphaPitch);

                float rAlpha, gAlpha, bAlpha, aAlpha, rImage, gImage, bImage, aImage;

                for (uint32 i = 0; i < pixelCount; ++i, imageBuf += imagePixelBytes, alphaBuf += alphaPixelBytes)
                {
                    alphaOp->GetRGBA(alphaBuf, rAlpha, gAlpha, bAlpha, aAlpha);
                    imageOp->GetRGBA(imageBuf, rImage, gImage, bImage, aImage);
                    imageOp->SetRGBA(imageBuf, rImage, gImage, bImage, aAlpha);
                }
            }
        }

        return previewImage;
    }

    IImageObjectPtr ConvertImageForPreview(IImageObjectPtr image)
    {
        if (!image)
        {
            return IImageObjectPtr();
        }

        ImageToProcess imageToProcess(image);
        imageToProcess.ConvertFormat(ePixelFormat_R8G8B8A8);
        IImageObjectPtr previewImage = imageToProcess.Get();

        return previewImage;
    }

    // This function will convert compressed image to RGBA32.
    // Also if the image is in sRGB space will convert it to Linear space.
    IImageObjectPtr GetUncompressedLinearImage(IImageObjectPtr ddsImage)
    {
        if (ddsImage)
        {
            ImageToProcess processImage(ddsImage);
            if (!CPixelFormats::GetInstance().IsPixelFormatUncompressed(ddsImage->GetPixelFormat()))
            {
                processImage.ConvertFormat(ePixelFormat_R32G32B32A32F);
            }
            if (ddsImage->HasImageFlags(EIF_SRGBRead))
            {
                processImage.GammaToLinearRGBA32F(true);
            }
            return processImage.Get();
        }
        return nullptr;
    }

    float GetErrorBetweenImages(IImageObjectPtr inputImage1, IImageObjectPtr inputImage2)
    {
        // First make sure images are in uncompressed format and linear space
        // Convert them if necessary
        IImageObjectPtr image1 = GetUncompressedLinearImage(inputImage1);
        IImageObjectPtr image2 = GetUncompressedLinearImage(inputImage2);

        const float errorValue = FLT_MAX;

        if (!image1 || !image2)
        {
            AZ_Warning("Image Processing", false, "Invalid images passed into %s function", __FUNCTION__);
            return errorValue;
        }

        // Two images should share same size
        if (image1->GetWidth(0) != image2->GetWidth(0) || image1->GetHeight(0) != image2->GetHeight(0))
        {
            AZ_Warning("Image Processing", false, "%s function only can get error between two images with same size", __FUNCTION__);
            return errorValue;
        }

        //create pixel operation function
        IPixelOperationPtr pixelOp1 = CreatePixelOperation(image1->GetPixelFormat());
        IPixelOperationPtr pixelOp2 = CreatePixelOperation(image2->GetPixelFormat());

        //get count of bytes per pixel
        AZ::u32 pixelBytes1 = CPixelFormats::GetInstance().GetPixelFormatInfo(image1->GetPixelFormat())->bitsPerBlock / 8;
        AZ::u32 pixelBytes2 = CPixelFormats::GetInstance().GetPixelFormatInfo(image2->GetPixelFormat())->bitsPerBlock / 8;

        float color1[4];
        float color2[4];
        AZ::u8* mem1;
        AZ::u8* mem2;
        uint32 pitch1, pitch2;

        float sumDeltaSqLinear = 0;

        //only process the highest mip
        image1->GetImagePointer(0, mem1, pitch1);
        image2->GetImagePointer(0, mem2, pitch2);

        const uint32 pixelCount = image1->GetPixelCount(0);

        for (uint32 i = 0; i < pixelCount; ++i)
        {
            pixelOp1->GetRGBA(mem1, color1[0], color1[1], color1[2], color1[3]);
            pixelOp2->GetRGBA(mem2, color2[0], color2[1], color2[2], color2[3]);

            sumDeltaSqLinear += (color1[0] - color2[0]) * (color1[0] - color2[0])
                + (color1[1] - color2[1]) * (color1[1] - color2[1])
                + (color1[2] - color2[2]) * (color1[2] - color2[2]);

            mem1 += pixelBytes1;
            mem2 += pixelBytes2;
        }

        return sumDeltaSqLinear / pixelCount;
    }

    void GetBC1CompressionErrors(IImageObjectPtr originImage, float& errorLinear, float& errorSrgb,
        ICompressor::CompressOption option)
    {
        errorLinear = 0;
        errorSrgb = 0;

        if (originImage->HasImageFlags(EIF_SRGBRead))
        {
            AZ_Assert(false, "The input origin image of %s function need be in linear color space", __FUNCTION__);
            return;
        }

        //compress and decompress in linear space
        ImageToProcess processLinear(originImage);
        processLinear.SetCompressOption(option);
        processLinear.ConvertFormat(ePixelFormat_BC1);
        processLinear.ConvertFormat(ePixelFormat_R32G32B32A32F);

        errorLinear = GetErrorBetweenImages(originImage, processLinear.Get());

        //compress and decompress in sRGB space, then convert back to linear space to compare to original image
        ImageToProcess processSrgb(originImage);
        processSrgb.SetCompressOption(option);
        processSrgb.LinearToGamma();
        processSrgb.ConvertFormat(ePixelFormat_BC1);
        processSrgb.ConvertFormat(ePixelFormat_R32G32B32A32F);
        processSrgb.GammaToLinearRGBA32F(true);

        errorSrgb = GetErrorBetweenImages(originImage, processSrgb.Get());
    }
}// namespace ImageProcessingAtom
