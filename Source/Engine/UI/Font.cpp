//
// Copyright (c) 2008-2013 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "AreaAllocator.h"
#include "Context.h"
#include "Deserializer.h"
#include "FileSystem.h"
#include "Font.h"
#include "Graphics.h"
#include "Log.h"
#include "MemoryBuffer.h"
#include "Profiler.h"
#include "ResourceCache.h"
#include "StringUtils.h"
#include "Texture2D.h"
#include "XMLFile.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include "DebugNew.h"

namespace Urho3D
{

static const int MIN_POINT_SIZE = 6;
static const int MAX_POINT_SIZE = 48;
static const int MAX_ASCII_CODE = 127;
static const int MIN_TEXTURE_SIZE = 128;
static const int MAX_TEXTURE_SIZE = 2048;

/// FreeType library subsystem.
class FreeTypeLibrary : public Object
{
    OBJECT(FreeTypeLibrary);

public:
    /// Construct.
    FreeTypeLibrary(Context* context) :
        Object(context)
    {
        FT_Error error = FT_Init_FreeType(&library_);
        if (error)
            LOGERROR("Could not initialize FreeType library");
    }

    /// Destruct.
    virtual ~FreeTypeLibrary()
    {
        for (List<FT_Face>::Iterator i = faceList_.Begin(); i != faceList_.End(); ++i)
            FT_Done_Face(*i);

        FT_Done_FreeType(library_);
    }

    /// Create face.
    FT_Face CreateFace(const unsigned char* fontData, unsigned fontDataSize, int pointSize)
    {
        FT_Face face;
        FT_Error error = FT_New_Memory_Face(library_, fontData, fontDataSize, 0, &face);
        if (error)
        {
            LOGERROR("Could not create font face");
            return 0;
        }
        
        const int fontDPI = 96;
        error = FT_Set_Char_Size(face, 0, pointSize * 64, fontDPI, fontDPI);
        if (error)
        {
            LOGERROR("Could not set font point size " + String(pointSize));
            FT_Done_Face(face);
            return 0;
        }

        faceList_.Push(face);

        return face;
    }

private:
    /// FreeType library.
    FT_Library library_;
    /// Face list.
    List<FT_Face> faceList_;
};

FontGlyph::FontGlyph()
{
}

FontFace::FontFace(Font* font, int pointSize) : font_(font),
    pointSize_(pointSize)
{
}

FontFace::~FontFace()
{

}

const FontGlyph* FontFace::GetGlyph(unsigned c) const
{
    HashMap<unsigned, FontGlyph>::ConstIterator i = glyphMapping_.Find(c);
    if (i != glyphMapping_.End())
        return &(i->second_);

    return 0;
}

short FontFace::GetKerning(unsigned c, unsigned d) const
{
    if (kerningMapping_.Empty())
        return 0;

    if (c == '\n' || d == '\n')
        return 0;

    HashMap<unsigned, short>::ConstIterator i = kerningMapping_.Find((c << 16) + d);
    if (i != kerningMapping_.End())
        return i->second_;

    return 0;
}

bool FontFace::IsDataLost() const
{
    for (unsigned i = 0; i < textures_.Size(); ++i)
    {
        if (textures_[i]->IsDataLost())
            return true;
    }
    return false;
}

unsigned FontFace::GetTotalTextureSize() const
{
    unsigned totalTextureSize = 0;
    for (unsigned i = 0; i < textures_.Size(); ++i)
        totalTextureSize += textures_[i]->GetWidth() * textures_[i]->GetHeight();

    return totalTextureSize;
}

MutableFontGlyph::MutableFontGlyph() : charCode_(0)
{

}

FontFaceTTF::FontFaceTTF(Font* font, int pointSize) : FontFace(font, pointSize),
    face_(0)
{

}

FontFaceTTF::~FontFaceTTF()
{
   for (List<MutableFontGlyph*>::Iterator i = mutableGlyphList.Begin(); i != mutableGlyphList.End(); ++i)
       delete (*i);
}

bool FontFaceTTF::Load(const unsigned char* fontData, unsigned fontDataSize)
{
    if (!font_)
        return false;

    if (pointSize_ <= 0)
    {
        LOGERROR("Zero or negative point size");
        return false;
    }

    if (fontDataSize == 0)
    {
        LOGERROR("Font not loaded");
        return false;
    }

    Context* context = font_->GetContext();

    // Create font face.
    FreeTypeLibrary* freeType = context->GetSubsystem<FreeTypeLibrary>();
    if (!freeType)
        context->RegisterSubsystem(freeType = new FreeTypeLibrary(context));

    FT_Face face = freeType->CreateFace(fontData, fontDataSize, pointSize_);
    if (!face)
        return false;

    face_ = face;
    rowHeight_ = (face->height * (face->size->metrics.y_scale >> 6)) >> 16;

    int texWidth;
    int texHeight;
    bool loadAllGlyphs = CalculateTextureSize(texWidth, texHeight);

    SharedArrayPtr<unsigned char> texData(new unsigned char[texWidth * texHeight]);
    for (int y = 0; y < texHeight; ++y)
    {
        unsigned char* dest = texData + texWidth * y;
        memset(dest, 0, texWidth);
    }

    bool hasKerning = FT_HAS_KERNING(face) != 0;
    HashMap<unsigned, unsigned> glyphIndexToCharCodeMapping;

    AreaAllocator allocator(texWidth, texHeight, texWidth, texHeight);

    FT_UInt glyphIndex;
    FT_ULong charCode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0)
    {
        if (loadAllGlyphs || charCode < MAX_ASCII_CODE)
        {
            FontGlyph glyph;
            FT_Error error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER);
            if (!error)
            {
                FT_GlyphSlot slot = face->glyph;
                FT_Pos ascender = face->size->metrics.ascender;

                glyph.width_ = (short)((slot->metrics.width) >> 6);
                glyph.height_ = (short)((slot->metrics.height) >> 6);
                glyph.offsetX_ = (short)((slot->metrics.horiBearingX) >> 6);
                glyph.offsetY_ = (short)((ascender - slot->metrics.horiBearingY) >> 6);
                glyph.advanceX_ = (short)((slot->metrics.horiAdvance) >> 6);
                glyph.page_ = 0;

                int x, y;
                if (allocator.Allocate(glyph.width_ + 1, glyph.height_ + 1, x, y))
                {
                    glyph.x_ = x;
                    glyph.y_ = y;
                }
                else
                {
                    LOGERROR("Allocate area failed");
                    return false;
                }

                if (glyph.width_ > 0 && glyph.height_ > 0)
                {
                    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                    {
                        for (int h = 0; h < glyph.height_; ++h)
                        {
                            unsigned char* src = slot->bitmap.buffer + slot->bitmap.pitch * h;
                            unsigned char* dest = texData + texWidth * (h + glyph.y_) + glyph.x_;
                            for (int w = 0; w < glyph.width_; ++w)
                                dest[w] = (src[w / 8] & (0x80 >> (w & 7))) ? 0xFF : 0x00;
                        }
                    }
                    else
                    {
                        for (int h = 0; h < glyph.height_; ++h)
                        {
                            unsigned char* src = slot->bitmap.buffer + slot->bitmap.pitch * h;
                            unsigned char* dest = texData + texWidth * (h + glyph.y_) + glyph.x_;
                            memcpy(dest, src, glyph.width_);
                        }
                    }
                }
            }
            else
            {
                glyph.x_ = 0;
                glyph.y_ = 0;
                glyph.width_ = 0;
                glyph.height_ = 0;
                glyph.offsetX_ = 0;
                glyph.offsetY_ = 0;
                glyph.advanceX_ = 0;
                glyph.page_ = 0;
            }

            glyphMapping_[charCode] = glyph;
        }       

        if (hasKerning)
            glyphIndexToCharCodeMapping[glyphIndex] = charCode;

        charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
    }

    // Create face texture
    SharedPtr<Texture2D> texture = CreateFaceTexture(texWidth, texHeight, texData);
    if (!texture)
        return false;
    textures_.Push(texture);

    // Build kerning mapping
    if (hasKerning)
    {
        FT_ULong tag = FT_MAKE_TAG('k', 'e', 'r', 'n');
        FT_ULong kerningTableSize = 0;
        FT_Error error = FT_Load_Sfnt_Table(face, tag, 0, NULL, &kerningTableSize);
        if (error)
        {
            LOGERROR("Could not get kerning table length");
            return false;
        }

        SharedArrayPtr<unsigned char> kerningTable(new unsigned char[kerningTableSize]);
        error = FT_Load_Sfnt_Table(face, tag, 0, kerningTable, &kerningTableSize);
        if (error)
        {
            LOGERROR("Could not load kerning table");
            return false;
        }

        // Freetype's buffer use big endian, need convert to little endian
        for (unsigned i = 0; i < kerningTableSize; i += 2)
            Swap(kerningTable[i], kerningTable[i + 1]);

        MemoryBuffer deserializer(kerningTable, kerningTableSize);

        unsigned short version = deserializer.ReadUShort();
        if (version != 0)
        {
            LOGERROR("Version error");
            return false;
        }

        float factor = face->size->metrics.x_ppem / (1.0f * face->units_per_EM);

        unsigned short numTables = deserializer.ReadUShort();
        for (int i = 0; i < numTables; ++i)
        {
            unsigned short version = deserializer.ReadUShort();
            unsigned short length = deserializer.ReadUShort();
            unsigned short coverage = deserializer.ReadUShort();
            if (version == 0 && coverage == 1)
            {
                unsigned short numPairs = deserializer.ReadUShort();
                for (int j = 0; j < numPairs; ++j)
                {
                    unsigned short leftGlyphIndex = deserializer.ReadUShort();
                    unsigned short rightGlyphIndex = deserializer.ReadUShort();
                    short amount = (short)(deserializer.ReadShort() * factor);
                    if (amount != 0)
                    {
                        unsigned leftCharCode = glyphIndexToCharCodeMapping[leftGlyphIndex];
                        unsigned rightCharCode = glyphIndexToCharCodeMapping[rightGlyphIndex];
                        kerningMapping_[(leftCharCode << 16) + rightCharCode] = amount;
                    }
                }
            }
            else
            {
                LOGERROR("Version or coverage error");
                return false;
            }
        }
    }

    // Allocate space for mutable glyph
    if (!loadAllGlyphs)
    {
        int x, y;
        while (allocator.Allocate(maxGlyphWidth_, maxGlyphHeight_, x, y))
        {
            MutableFontGlyph* glyph = new MutableFontGlyph;
            glyph->x_ = x;
            glyph->y_ = y;
            glyph->page_ = 0;
            glyph->charCode_ = 0;

            mutableGlyphList.PushFront(glyph);
            glyph->iterator_ = mutableGlyphList.Begin();
        }
    }

    return true;
}

const FontGlyph* FontFaceTTF::GetGlyph(unsigned c) const
{
    if (mutableGlyphList.Empty() || c <= MAX_ASCII_CODE)
        return FontFace::GetGlyph(c);

    HashMap<unsigned, MutableFontGlyph*>::ConstIterator i = mutableGlyphMapping_.Find(c);
    if (i != mutableGlyphMapping_.End())
    {
        MutableFontGlyph* glyph = i->second_;

        mutableGlyphList.Erase(glyph->iterator_);
        mutableGlyphList.PushFront(glyph);
        glyph->iterator_ = mutableGlyphList.Begin();

        return glyph;
    }

    FT_Face face = (FT_Face)face_;
    FT_GlyphSlot slot = face->glyph;
    FT_Pos ascender = face->size->metrics.ascender;
    FT_Error error = FT_Load_Char(face, c, FT_LOAD_RENDER);
    if (error)
        return 0;

    MutableFontGlyph* glyph = mutableGlyphList.Back();
    mutableGlyphList.Erase(glyph->iterator_);
    mutableGlyphList.PushFront(glyph);
    glyph->iterator_ = mutableGlyphList.Begin();

    if (glyph->charCode_ != 0)
        mutableGlyphMapping_.Erase(glyph->charCode_);
    glyph->charCode_ = c;
    mutableGlyphMapping_[glyph->charCode_] = glyph;

    glyph->width_ = (short)((slot->metrics.width) >> 6);
    glyph->height_ = (short)((slot->metrics.height) >> 6);
    glyph->offsetX_ = (short)((slot->metrics.horiBearingX) >> 6);
    glyph->offsetY_ = (short)((ascender - slot->metrics.horiBearingY) >> 6);
    glyph->advanceX_ = (short)((slot->metrics.horiAdvance) >> 6);

    SharedArrayPtr<unsigned char> data(new unsigned char[maxGlyphWidth_ * maxGlyphHeight_]);
    memset(data, 0, maxGlyphWidth_ * maxGlyphHeight_);

    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
    {
        for (int y = 0; y < glyph->height_; ++y)
        {
            unsigned char* src = slot->bitmap.buffer + slot->bitmap.pitch * y;
            unsigned char* dest = data + maxGlyphWidth_ * y;
            for (int w = 0; w < glyph->width_; ++w)
                dest[w] = (src[w / 8] & (0x80 >> (w & 7))) ? 0xFF : 0x00;
        }
    }
    else
    {
        for (int y = 0; y < glyph->height_; ++y)
        {
            unsigned char* src = slot->bitmap.buffer + slot->bitmap.pitch * y;
            unsigned char* dest = data + maxGlyphWidth_ * y;
            memcpy(dest, src, glyph->width_);
        }
    }

    textures_[0]->SetData(0, glyph->x_, glyph->y_, maxGlyphWidth_, maxGlyphHeight_, data);

    return glyph;
}

bool FontFaceTTF::CalculateTextureSize(int &texWidth, int &texHeight)
{
    bool loadAllGlyphs = true;
    
    FT_Face face = (FT_Face)face_;

    int maxTexWidth = MAX_TEXTURE_SIZE;
    int maxTexHeight = MAX_TEXTURE_SIZE;
    if (pointSize_ < 32)
        maxTexWidth /= 2;
    if (pointSize_ < 22)
        maxTexHeight /= 2;
    if (pointSize_ < 16)
        maxTexWidth /= 2;
    if (pointSize_ < 11)
        maxTexHeight /= 2;

    AreaAllocator allocator(MIN_TEXTURE_SIZE, MIN_TEXTURE_SIZE, maxTexWidth, maxTexHeight);

    FT_UInt glyphIndex;
    FT_ULong charCode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0)
    {
        FT_Error error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT);
        if (error)
        {
            charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
            continue;
        }

        int width = ((face->glyph->metrics.width) >> 6);
        int height = ((face->glyph->metrics.height) >> 6);

        if (loadAllGlyphs)
        {
            int x, y;
            if (!allocator.Allocate(width + 1, height + 1, x, y))
                loadAllGlyphs = false;
        }

        maxGlyphWidth_ = Max(maxGlyphWidth_, width + 1);
        maxGlyphHeight_ = Max(maxGlyphHeight_, height + 1);

        charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
    }

    texWidth = allocator.GetWidth();
    texHeight = allocator.GetHeight();

    return loadAllGlyphs;
}

SharedPtr<Texture2D> FontFaceTTF::CreateFaceTexture(int texWidth, int texHeight, unsigned char* texData)
{
    if (texWidth == 0 || texHeight == 0 || !texData)
        return SharedPtr<Texture2D>();

    Graphics* graphcs = font_->GetContext()->GetSubsystem<Graphics>();
    if (!graphcs)
        return SharedPtr<Texture2D>();

    SharedPtr<Texture2D> texture(new Texture2D(font_->GetContext()));
    texture->SetMipsToSkip(QUALITY_LOW, 0);
    texture->SetNumLevels(1);
    if (!texture->SetSize(texWidth, texHeight, graphcs->GetAlphaFormat()))
    {
        LOGERROR("Could not set texture size");
        return SharedPtr<Texture2D>();
    }

    if (!texture->SetData(0, 0, 0, texWidth, texHeight, texData))
    {
        LOGERROR("Could not set texture data");
        return SharedPtr<Texture2D>();
    }

    return texture;
}

FontFaceBitmap::FontFaceBitmap(Font* font, int pointSize) : FontFace(font, pointSize)
{

}

FontFaceBitmap::~FontFaceBitmap()
{

}

bool FontFaceBitmap::Load(const unsigned char* fontData, unsigned fontDataSize)
{
    if (!font_)
        return false;

    Context* context_ = font_->GetContext();

    SharedPtr<XMLFile> xmlReader(new XMLFile(context_));
    MemoryBuffer memoryBuffer(fontData, fontDataSize);
    if (!xmlReader->Load(memoryBuffer))
    {
        LOGERROR("Could not load XML file");
        return false;
    }

    XMLElement root = xmlReader->GetRoot("font");
    if (root.IsNull())
    {
        LOGERROR("Could not find Font element");
        return false;
    }

    XMLElement pagesElem = root.GetChild("pages");
    if (pagesElem.IsNull())
    {
        LOGERROR("Could not find Pages element");
        return false;
    }

    XMLElement infoElem = root.GetChild("info");
    if (!infoElem.IsNull())
        pointSize_ = infoElem.GetInt("size");

    XMLElement commonElem = root.GetChild("common");
    rowHeight_ = commonElem.GetInt("lineHeight");
    unsigned pages = commonElem.GetInt("pages");
    textures_.Reserve(pages);

    ResourceCache* resourceCache = context_->GetSubsystem<ResourceCache>();
    String fontPath = GetPath(font_->GetName());
    unsigned totalTextureSize = 0;

    XMLElement pageElem = pagesElem.GetChild("page");
    for (unsigned i = 0; i < pages; ++i)
    {
        if (pageElem.IsNull())
        {
            LOGERROR("Could not find Page element for page: " + String(i));
            return false;
        }

        // Assume the font image is in the same directory as the font description file
        String textureFile = fontPath + pageElem.GetAttribute("file");

        // Load texture manually to allow controlling the alpha channel mode
        SharedPtr<File> fontFile = resourceCache->GetFile(textureFile);
        SharedPtr<Image> fontImage(new Image(context_));
        if (!fontFile || !fontImage->Load(*fontFile))
        {
            LOGERROR("Failed to load font image file");
            return false;
        }

        SharedPtr<Texture2D> texture = CreateFaceTexture(fontImage);
        if (!texture)
            return false;
        textures_.Push(texture);
        totalTextureSize += fontImage->GetWidth() * fontImage->GetHeight() * fontImage->GetComponents();
        pageElem = pageElem.GetNext("page");
    }

    XMLElement charsElem = root.GetChild("chars");
    int count = charsElem.GetInt("count");
    unsigned index = 0;

    XMLElement charElem = charsElem.GetChild("char");
    while (!charElem.IsNull())
    {
        int id = charElem.GetInt("id");
        FontGlyph glyph;

        glyph.x_ = charElem.GetInt("x");
        glyph.y_ = charElem.GetInt("y");
        glyph.width_ = charElem.GetInt("width");
        glyph.height_ = charElem.GetInt("height");
        glyph.offsetX_ = charElem.GetInt("xoffset");
        glyph.offsetY_ = charElem.GetInt("yoffset");
        glyph.advanceX_ = charElem.GetInt("xadvance");
        glyph.page_ = charElem.GetInt("page");
        glyphMapping_[id] = glyph;

        charElem = charElem.GetNext("char");
    }

    XMLElement kerningsElem = root.GetChild("kernings");
    if (kerningsElem.NotNull())
    {
        XMLElement kerningElem = kerningsElem.GetChild("kerning");
        while (!kerningElem.IsNull())
        {
            int first = kerningElem.GetInt("first");
            int second = kerningElem.GetInt("second");
            int amount = kerningElem.GetInt("amount");
            if (amount != 0)
            {
                unsigned key = (first << 16) + second;
                kerningMapping_[key] = (short)amount;
            }

            kerningElem = kerningElem.GetNext("kerning");
        }
    }

    LOGDEBUG(ToString("Bitmap font face %s has %d glyphs", GetFileName(font_->GetName()).CString(), count));

    return true;
}

SharedPtr<Texture2D> FontFaceBitmap::CreateFaceTexture(SharedPtr<Image> image)
{
    SharedPtr<Texture2D> texture(new Texture2D(font_->GetContext()));
    texture->SetMipsToSkip(QUALITY_LOW, 0);
    texture->SetNumLevels(1);
    if (!texture->Load(image, true))
    {
        LOGERROR("Could not load texture from image resource");
        return SharedPtr<Texture2D>();
    }
    return SharedPtr<Texture2D>(texture);
}

Font::Font(Context* context) :
    Resource(context),
    fontDataSize_(0),
    fontType_(FONT_NONE)
{
}

Font::~Font()
{
}

void Font::RegisterObject(Context* context)
{
    context->RegisterFactory<Font>();
}

bool Font::Load(Deserializer& source)
{
    PROFILE(LoadFont);

    // In headless mode, do not actually load, just return success
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return true;

    faces_.Clear();

    fontDataSize_ = source.GetSize();
    if (fontDataSize_)
    {
        fontData_ = new unsigned char[fontDataSize_];
        if (source.Read(&fontData_[0], fontDataSize_) != fontDataSize_)
            return false;
    }
    else
    {
        fontData_.Reset();
        return false;
    }

    String ext = GetExtension(GetName());
    if (ext == ".ttf")
        fontType_ = FONT_TTF;
    else if (ext == ".xml" || ext == ".fnt")
        fontType_ = FONT_BITMAP;

    SetMemoryUse(fontDataSize_);
    return true;
}

const FontFace* Font::GetFace(int pointSize)
{
    // In headless mode, always return null
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return 0;

    // For bitmap font type, always return the same font face provided by the font's bitmap file regardless of the actual requested point size
    if (fontType_ == FONT_BITMAP)
        pointSize = 0;
    else
        pointSize = Clamp(pointSize, MIN_POINT_SIZE, MAX_POINT_SIZE);

    HashMap<int, SharedPtr<FontFace> >::Iterator i = faces_.Find(pointSize);
    if (i != faces_.End())
    {
        if (!i->second_->IsDataLost())
            return i->second_;
        else
        {
            // Erase and reload face if texture data lost (OpenGL mode only)
            faces_.Erase(i);
        }
    }

    PROFILE(GetFontFace);

    switch (fontType_)
    {
    case FONT_TTF:
        return GetFaceTTF(pointSize);

    case FONT_BITMAP:
        return GetFaceBitmap(pointSize);

    default:
        return 0;
    }
}

const FontFace* Font::GetFaceTTF(int pointSize)
{
    SharedPtr<FontFace> newFace(new FontFaceTTF(this, pointSize));
    if (!newFace->Load(fontData_, fontDataSize_))
        return 0;

    SetMemoryUse(GetMemoryUse() + newFace->GetTotalTextureSize());
    faces_[pointSize] = newFace;
    return newFace;
}

const FontFace* Font::GetFaceBitmap(int pointSize)
{
    SharedPtr<FontFace> newFace(new FontFaceBitmap(this, pointSize));
    if (!newFace->Load(fontData_, fontDataSize_))
        return 0;

    SetMemoryUse(GetMemoryUse() + newFace->GetTotalTextureSize());
    faces_[pointSize] = newFace;
    return newFace;
}

}
