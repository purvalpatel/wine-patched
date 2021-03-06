/*
 * Unit test suite for metafiles
 *
 * Copyright (C) 2011 Vincent Povirk for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>

#include "objbase.h"
#include "gdiplus.h"
#include "wine/test.h"

#define expect(expected, got) ok(got == expected, "Expected %.8x, got %.8x\n", expected, got)
#define expectf_(expected, got, precision) ok(fabs((expected) - (got)) <= (precision), "Expected %f, got %f\n", (expected), (got))
#define expectf(expected, got) expectf_((expected), (got), 0.001)

static BOOL save_metafiles;
static BOOL load_metafiles;

typedef struct emfplus_record
{
    BOOL  todo;
    ULONG record_type;
    BOOL  playback_todo;
    void (*playback_fn)(GpMetafile* metafile, EmfPlusRecordType record_type,
        unsigned int flags, unsigned int dataSize, const unsigned char *pStr);
} emfplus_record;

typedef struct emfplus_check_state
{
    const char *desc;
    int count;
    const struct emfplus_record *expected;
    GpMetafile *metafile;
} emfplus_check_state;

static void check_record(int count, const char *desc, const struct emfplus_record *expected, const struct emfplus_record *actual)
{
    todo_wine_if (expected->todo)
        ok(expected->record_type == actual->record_type,
            "%s.%i: Expected record type 0x%x, got 0x%x\n", desc, count,
            expected->record_type, actual->record_type);
}

typedef struct EmfPlusRecordHeader
{
    WORD Type;
    WORD Flags;
    DWORD Size;
    DWORD DataSize;
} EmfPlusRecordHeader;

typedef enum
{
    ObjectTypeInvalid,
    ObjectTypeBrush,
    ObjectTypePen,
    ObjectTypePath,
    ObjectTypeRegion,
    ObjectTypeImage,
    ObjectTypeFont,
    ObjectTypeStringFormat,
    ObjectTypeImageAttributes,
    ObjectTypeCustomLineCap,
} ObjectType;

typedef enum
{
    ImageDataTypeUnknown,
    ImageDataTypeBitmap,
    ImageDataTypeMetafile,
} ImageDataType;

typedef struct
{
    EmfPlusRecordHeader Header;
    /* EmfPlusImage */
    DWORD Version;
    ImageDataType Type;
    /* EmfPlusMetafile */
    DWORD MetafileType;
    DWORD MetafileDataSize;
    BYTE MetafileData[1];
} MetafileImageObject;

static int CALLBACK enum_emf_proc(HDC hDC, HANDLETABLE *lpHTable, const ENHMETARECORD *lpEMFR,
    int nObj, LPARAM lpData)
{
    emfplus_check_state *state = (emfplus_check_state*)lpData;
    emfplus_record actual;

    if (lpEMFR->iType == EMR_GDICOMMENT)
    {
        const EMRGDICOMMENT *comment = (const EMRGDICOMMENT*)lpEMFR;

        if (comment->cbData >= 4 && memcmp(comment->Data, "EMF+", 4) == 0)
        {
            int offset = 4;

            while (offset + sizeof(EmfPlusRecordHeader) <= comment->cbData)
            {
                const EmfPlusRecordHeader *record = (const EmfPlusRecordHeader*)&comment->Data[offset];

                ok(record->Size == record->DataSize + sizeof(EmfPlusRecordHeader),
                    "%s: EMF+ record datasize %u and size %u mismatch\n", state->desc, record->DataSize, record->Size);

                ok(offset + record->DataSize <= comment->cbData,
                    "%s: EMF+ record truncated\n", state->desc);

                if (offset + record->DataSize > comment->cbData)
                    return 0;

                if (state->expected[state->count].record_type)
                {
                    actual.todo = FALSE;
                    actual.record_type = record->Type;

                    check_record(state->count, state->desc, &state->expected[state->count], &actual);
                    state->count++;

                    if (state->expected[state->count-1].todo && state->expected[state->count-1].record_type != actual.record_type)
                        continue;
                }
                else
                {
                    ok(0, "%s: Unexpected EMF+ 0x%x record\n", state->desc, record->Type);
                }

                if ((record->Flags >> 8) == ObjectTypeImage && record->Type == EmfPlusRecordTypeObject)
                {
                    const MetafileImageObject *image = (const MetafileImageObject*)record;

                    if (image->Type == ImageDataTypeMetafile)
                    {
                        HENHMETAFILE hemf = SetEnhMetaFileBits(image->MetafileDataSize, image->MetafileData);
                        ok(hemf != NULL, "%s: SetEnhMetaFileBits failed\n", state->desc);

                        EnumEnhMetaFile(0, hemf, enum_emf_proc, state, NULL);
                        DeleteEnhMetaFile(hemf);
                    }
                }

                offset += record->Size;
            }

            ok(offset == comment->cbData, "%s: truncated EMF+ record data?\n", state->desc);

            return 1;
        }
    }

    if (state->expected[state->count].record_type)
    {
        actual.todo = FALSE;
        actual.record_type = lpEMFR->iType;

        check_record(state->count, state->desc, &state->expected[state->count], &actual);

        state->count++;
    }
    else
    {
        ok(0, "%s: Unexpected EMF 0x%x record\n", state->desc, lpEMFR->iType);
    }

    return 1;
}

static void check_emfplus(HENHMETAFILE hemf, const emfplus_record *expected, const char *desc)
{
    emfplus_check_state state;

    state.desc = desc;
    state.count = 0;
    state.expected = expected;

    EnumEnhMetaFile(0, hemf, enum_emf_proc, &state, NULL);

    todo_wine_if (expected[state.count].todo)
        ok(expected[state.count].record_type == 0, "%s: Got %i records, expecting more\n", desc, state.count);
}

static BOOL CALLBACK enum_metafile_proc(EmfPlusRecordType record_type, unsigned int flags,
    unsigned int dataSize, const unsigned char *pStr, void *userdata)
{
    emfplus_check_state *state = (emfplus_check_state*)userdata;
    emfplus_record actual;

    actual.todo = FALSE;
    actual.record_type = record_type;

    if (dataSize == 0)
        ok(pStr == NULL, "non-NULL pStr\n");

    if (state->expected[state->count].record_type)
    {
        check_record(state->count, state->desc, &state->expected[state->count], &actual);

        state->count++;
    }
    else
    {
        ok(0, "%s: Unexpected EMF 0x%x record\n", state->desc, record_type);
    }

    return TRUE;
}

static void check_metafile(GpMetafile *metafile, const emfplus_record *expected, const char *desc,
    const GpPointF *dst_points, const GpRectF *src_rect, Unit src_unit)
{
    GpStatus stat;
    HDC hdc;
    GpGraphics *graphics;
    emfplus_check_state state;

    state.desc = desc;
    state.count = 0;
    state.expected = expected;
    state.metafile = metafile;

    hdc = CreateCompatibleDC(0);

    stat = GdipCreateFromHDC(hdc, &graphics);
    expect(Ok, stat);

    stat = GdipEnumerateMetafileSrcRectDestPoints(graphics, metafile, dst_points,
        3, src_rect, src_unit, enum_metafile_proc, &state, NULL);
    expect(Ok, stat);

    todo_wine_if (expected[state.count].todo)
        ok(expected[state.count].record_type == 0, "%s: Got %i records, expecting more\n", desc, state.count);

    GdipDeleteGraphics(graphics);

    DeleteDC(hdc);
}

static BOOL CALLBACK play_metafile_proc(EmfPlusRecordType record_type, unsigned int flags,
    unsigned int dataSize, const unsigned char *pStr, void *userdata)
{
    emfplus_check_state *state = (emfplus_check_state*)userdata;
    GpStatus stat;

    if (state->expected[state->count].record_type)
    {
        BOOL match = (state->expected[state->count].record_type == record_type);

        if (match && state->expected[state->count].playback_fn)
            state->expected[state->count].playback_fn(state->metafile, record_type, flags, dataSize, pStr);
        else
        {
            stat = GdipPlayMetafileRecord(state->metafile, record_type, flags, dataSize, pStr);
            todo_wine_if (state->expected[state->count].playback_todo)
                ok(stat == Ok, "%s.%i: GdipPlayMetafileRecord failed with stat %i\n", state->desc, state->count, stat);
        }

        todo_wine_if (state->expected[state->count].todo)
            ok(state->expected[state->count].record_type == record_type,
                "%s.%i: expected record type 0x%x, got 0x%x\n", state->desc, state->count,
                state->expected[state->count].record_type, record_type);
        state->count++;
    }
    else
    {
        todo_wine_if (state->expected[state->count].playback_todo)
            ok(0, "%s: unexpected record 0x%x\n", state->desc, record_type);

        return FALSE;
    }

    return TRUE;
}

static void play_metafile(GpMetafile *metafile, GpGraphics *graphics, const emfplus_record *expected,
    const char *desc, const GpPointF *dst_points, const GpRectF *src_rect, Unit src_unit)
{
    GpStatus stat;
    emfplus_check_state state;

    state.desc = desc;
    state.count = 0;
    state.expected = expected;
    state.metafile = metafile;

    stat = GdipEnumerateMetafileSrcRectDestPoints(graphics, metafile, dst_points,
        3, src_rect, src_unit, play_metafile_proc, &state, NULL);
    expect(Ok, stat);
}

/* When 'save' or 'load' is specified on the command line, save or
 * load the specified filename. */
static void sync_metafile(GpMetafile **metafile, const char *filename)
{
    GpStatus stat;
    if (save_metafiles)
    {
        GpMetafile *clone;
        HENHMETAFILE hemf;

        stat = GdipCloneImage((GpImage*)*metafile, (GpImage**)&clone);
        expect(Ok, stat);

        stat = GdipGetHemfFromMetafile(clone, &hemf);
        expect(Ok, stat);

        DeleteEnhMetaFile(CopyEnhMetaFileA(hemf, filename));

        DeleteEnhMetaFile(hemf);

        stat = GdipDisposeImage((GpImage*)clone);
        expect(Ok, stat);
    }
    else if (load_metafiles)
    {
        HENHMETAFILE hemf;

        stat = GdipDisposeImage((GpImage*)*metafile);
        expect(Ok, stat);
        *metafile = NULL;

        hemf = GetEnhMetaFileA(filename);
        ok(hemf != NULL, "%s could not be opened\n", filename);

        stat = GdipCreateMetafileFromEmf(hemf, TRUE, metafile);
        expect(Ok, stat);
    }
}

static const emfplus_record empty_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_empty(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    GpRectF bounds;
    GpUnit unit;
    REAL xres, yres;
    HENHMETAFILE hemf, dummy;
    MetafileHeader header;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(NULL, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, MetafileTypeInvalid, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, MetafileTypeWmf, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, MetafileTypeWmfPlaceable, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, MetafileTypeEmfPlusDual+1, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, NULL);
    expect(InvalidParameter, stat);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, empty_records, "empty metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "empty.emf");

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    expectf_(100.0, bounds.Width, 0.05);
    expectf_(100.0, bounds.Height, 0.05);
    expect(UnitPixel, unit);

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &xres);
    expect(Ok, stat);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &yres);
    expect(Ok, stat);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmfPlusOnly, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);
    expect(1, header.EmfPlusFlags); /* reference device was display, not printer */
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(28, header.EmfPlusHeaderSize);
    expect(96, header.LogicalDpiX);
    expect(96, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(0, U(header).EmfHeader.rclBounds.left);
    expect(0, U(header).EmfHeader.rclBounds.top);
    expect(-1, U(header).EmfHeader.rclBounds.right);
    expect(-1, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    stat = GdipGetHemfFromMetafile(metafile, &dummy);
    expect(InvalidParameter, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    check_emfplus(hemf, empty_records, "empty emf");

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromEmf(hemf, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmfPlusOnly, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);
    expect(1, header.EmfPlusFlags); /* reference device was display, not printer */
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(28, header.EmfPlusHeaderSize);
    expect(96, header.LogicalDpiX);
    expect(96, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(0, U(header).EmfHeader.rclBounds.left);
    expect(0, U(header).EmfHeader.rclBounds.top);
    expect(-1, U(header).EmfHeader.rclBounds.right);
    expect(-1, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipCreateMetafileFromEmf(hemf, TRUE, &metafile);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    expectf_(100.0, bounds.Width, 0.05);
    expectf_(100.0, bounds.Height, 0.05);
    expect(UnitPixel, unit);

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &xres);
    expect(Ok, stat);
    expectf(header.DpiX, xres);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &yres);
    expect(Ok, stat);
    expectf(header.DpiY, yres);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmfPlusOnly, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);
    expect(1, header.EmfPlusFlags); /* reference device was display, not printer */
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(28, header.EmfPlusHeaderSize);
    expect(96, header.LogicalDpiX);
    expect(96, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(0, U(header).EmfHeader.rclBounds.left);
    expect(0, U(header).EmfHeader.rclBounds.top);
    expect(-1, U(header).EmfHeader.rclBounds.right);
    expect(-1, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record getdc_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeGetDC},
    {0, EMR_CREATEBRUSHINDIRECT},
    {0, EMR_SELECTOBJECT},
    {0, EMR_RECTANGLE},
    {0, EMR_SELECTOBJECT},
    {0, EMR_DELETEOBJECT},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_getdc(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc, metafile_dc;
    HENHMETAFILE hemf;
    BOOL ret;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const GpPointF dst_points_half[3] = {{0.0,0.0},{50.0,0.0},{0.0,50.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    HBRUSH hbrush, holdbrush;
    GpBitmap *bitmap;
    ARGB color;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetDC(graphics, &metafile_dc);
    expect(Ok, stat);

    if (stat != Ok)
    {
        GdipDeleteGraphics(graphics);
        GdipDisposeImage((GpImage*)metafile);
        return;
    }

    hbrush = CreateSolidBrush(0xff0000);

    holdbrush = SelectObject(metafile_dc, hbrush);

    Rectangle(metafile_dc, 25, 25, 75, 75);

    SelectObject(metafile_dc, holdbrush);

    DeleteObject(hbrush);

    stat = GdipReleaseDC(graphics, metafile_dc);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, getdc_records, "getdc metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "getdc.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, getdc_records, "getdc playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapSetPixel(bitmap, 50, 50, 0);
    expect(Ok, stat);

    play_metafile(metafile, graphics, getdc_records, "getdc playback", dst_points_half, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapSetPixel(bitmap, 15, 15, 0);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, (GpImage*)metafile, dst_points, 3,
        0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
    expect(Ok, stat);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    check_emfplus(hemf, getdc_records, "getdc emf");

    ret = DeleteEnhMetaFile(hemf);
    ok(ret != 0, "Failed to delete enhmetafile %p\n", hemf);
}

static const emfplus_record emfonly_records[] = {
    {0, EMR_HEADER},
    {0, EMR_CREATEBRUSHINDIRECT},
    {0, EMR_SELECTOBJECT},
    {0, EMR_RECTANGLE},
    {0, EMR_SELECTOBJECT},
    {0, EMR_DELETEOBJECT},
    {0, EMR_EOF},
    {0}
};

static const emfplus_record emfonly_draw_records[] = {
    {0, EMR_HEADER},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_SETMITERLIMIT},
    {1, EMR_MODIFYWORLDTRANSFORM},
    {1, EMR_EXTCREATEPEN},
    {1, EMR_SELECTOBJECT},
    {1, EMR_SELECTOBJECT},
    {1, EMR_POLYLINE16},
    {1, EMR_SELECTOBJECT},
    {1, EMR_SELECTOBJECT},
    {1, EMR_MODIFYWORLDTRANSFORM},
    {1, EMR_DELETEOBJECT},
    {1, EMR_SETMITERLIMIT},
    {1, EMR_RESTOREDC},
    {0, EMR_EOF},
    {1}
};

static void test_emfonly(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpImage *clone;
    GpGraphics *graphics;
    HDC hdc, metafile_dc;
    GpRectF bounds;
    GpUnit unit;
    REAL xres, yres;
    HENHMETAFILE hemf;
    MetafileHeader header;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    HBRUSH hbrush, holdbrush;
    GpBitmap *bitmap;
    ARGB color;
    GpPen *pen;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmf, header.Type);
    ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);
    /* The rest is zeroed or seemingly random/uninitialized garbage. */

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetDC(graphics, &metafile_dc);
    expect(Ok, stat);

    if (stat != Ok)
    {
        GdipDeleteGraphics(graphics);
        GdipDisposeImage((GpImage*)metafile);
        return;
    }

    hbrush = CreateSolidBrush(0xff0000);

    holdbrush = SelectObject(metafile_dc, hbrush);

    Rectangle(metafile_dc, 25, 25, 75, 75);

    SelectObject(metafile_dc, holdbrush);

    DeleteObject(hbrush);

    stat = GdipReleaseDC(graphics, metafile_dc);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, emfonly_records, "emfonly metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "emfonly.emf");

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    expectf_(100.0, bounds.Width, 0.05);
    expectf_(100.0, bounds.Height, 0.05);
    expect(UnitPixel, unit);

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &xres);
    expect(Ok, stat);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &yres);
    expect(Ok, stat);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmf, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    /* For some reason a recoreded EMF Metafile has an EMF+ version. */
    todo_wine ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);
    expect(0, header.EmfPlusFlags);
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(0, header.EmfPlusHeaderSize);
    expect(0, header.LogicalDpiX);
    expect(0, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(25, U(header).EmfHeader.rclBounds.left);
    expect(25, U(header).EmfHeader.rclBounds.top);
    expect(74, U(header).EmfHeader.rclBounds.right);
    expect(74, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, emfonly_records, "emfonly playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapSetPixel(bitmap, 50, 50, 0);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, (GpImage*)metafile, dst_points, 3,
        0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
    expect(Ok, stat);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipCloneImage((GpImage*)metafile, &clone);
    expect(Ok, stat);

    if (stat == Ok)
    {
        stat = GdipBitmapSetPixel(bitmap, 50, 50, 0);
        expect(Ok, stat);

        stat = GdipDrawImagePointsRect(graphics, clone, dst_points, 3,
            0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
        expect(Ok, stat);

        stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
        expect(Ok, stat);
        expect(0, color);

        stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
        expect(Ok, stat);
        expect(0xff0000ff, color);

        GdipDisposeImage(clone);
    }

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    check_emfplus(hemf, emfonly_records, "emfonly emf");

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromEmf(hemf, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmf, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    expect(0x10000, header.Version);
    expect(0, header.EmfPlusFlags);
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(0, header.EmfPlusHeaderSize);
    expect(0, header.LogicalDpiX);
    expect(0, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(25, U(header).EmfHeader.rclBounds.left);
    expect(25, U(header).EmfHeader.rclBounds.top);
    expect(74, U(header).EmfHeader.rclBounds.right);
    expect(74, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipCreateMetafileFromEmf(hemf, TRUE, &metafile);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    expectf_(100.0, bounds.Width, 0.05);
    expectf_(100.0, bounds.Height, 0.05);
    expect(UnitPixel, unit);

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &xres);
    expect(Ok, stat);
    expectf(header.DpiX, xres);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &yres);
    expect(Ok, stat);
    expectf(header.DpiY, yres);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmf, header.Type);
    expect(U(header).EmfHeader.nBytes, header.Size);
    expect(0x10000, header.Version);
    expect(0, header.EmfPlusFlags);
    expectf(xres, header.DpiX);
    expectf(xres, U(header).EmfHeader.szlDevice.cx / (REAL)U(header).EmfHeader.szlMillimeters.cx * 25.4);
    expectf(yres, header.DpiY);
    expectf(yres, U(header).EmfHeader.szlDevice.cy / (REAL)U(header).EmfHeader.szlMillimeters.cy * 25.4);
    expect(0, header.X);
    expect(0, header.Y);
    expect(100, header.Width);
    expect(100, header.Height);
    expect(0, header.EmfPlusHeaderSize);
    expect(0, header.LogicalDpiX);
    expect(0, header.LogicalDpiX);
    expect(EMR_HEADER, U(header).EmfHeader.iType);
    expect(25, U(header).EmfHeader.rclBounds.left);
    expect(25, U(header).EmfHeader.rclBounds.top);
    expect(74, U(header).EmfHeader.rclBounds.right);
    expect(74, U(header).EmfHeader.rclBounds.bottom);
    expect(0, U(header).EmfHeader.rclFrame.left);
    expect(0, U(header).EmfHeader.rclFrame.top);
    expectf_(100.0, U(header).EmfHeader.rclFrame.right * xres / 2540.0, 2.0);
    expectf_(100.0, U(header).EmfHeader.rclFrame.bottom * yres / 2540.0, 2.0);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    /* test drawing to metafile with gdi+ functions */
    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreatePen1((ARGB)0xffff00ff, 10.0f, UnitPixel, &pen);
    expect(Ok, stat);
    stat = GdipDrawLineI(graphics, pen, 0, 0, 10, 10);
    todo_wine expect(Ok, stat);
    GdipDeletePen(pen);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, emfonly_draw_records, "emfonly draw metafile", dst_points, &frame, UnitPixel);
    sync_metafile(&metafile, "emfonly_draw.emf");

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record fillrect_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_fillrect(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    HENHMETAFILE hemf;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const GpPointF dst_points_half[3] = {{0.0,0.0},{50.0,0.0},{0.0,50.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpBitmap *bitmap;
    ARGB color;
    GpBrush *brush;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 25, 25, 75, 75);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, fillrect_records, "fillrect metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "fillrect.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, fillrect_records, "fillrect playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapSetPixel(bitmap, 50, 50, 0);
    expect(Ok, stat);

    play_metafile(metafile, graphics, fillrect_records, "fillrect playback", dst_points_half, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapSetPixel(bitmap, 15, 15, 0);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, (GpImage*)metafile, dst_points, 3,
        0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
    expect(Ok, stat);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record clear_emf_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeClear},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_clear(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    HENHMETAFILE hemf;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{10.0,10.0},{20.0,10.0},{10.0,20.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpBitmap *bitmap;
    ARGB color;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGraphicsClear(graphics, 0xffffff00);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    sync_metafile(&metafile, "clear.emf");

    stat = GdipCreateBitmapFromScan0(30, 30, 0, PixelFormat32bppRGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, (GpImage*)metafile, dst_points, 3,
        0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
    expect(Ok, stat);

    stat = GdipBitmapGetPixel(bitmap, 5, 5, &color);
    expect(Ok, stat);
    expect(0xff000000, color);

    stat = GdipBitmapGetPixel(bitmap, 15, 15, &color);
    expect(Ok, stat);
    expect(0xffffff00, color);

    stat = GdipBitmapGetPixel(bitmap, 25, 25, &color);
    expect(Ok, stat);
    expect(0xff000000, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    check_emfplus(hemf, clear_emf_records, "clear emf");

    DeleteEnhMetaFile(hemf);
}

static void test_nullframerect(void) {
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc, metafile_dc;
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpBrush *brush;
    HBRUSH hbrush, holdbrush;
    GpRectF bounds;
    GpUnit unit;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, NULL, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    ok(bounds.Width == 1.0 || broken(bounds.Width == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Width);
    ok(bounds.Height == 1.0 || broken(bounds.Height == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Height);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 25, 25, 75, 75);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    ok(bounds.Width == 1.0 || broken(bounds.Width == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Width);
    ok(bounds.Height == 1.0 || broken(bounds.Height == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Height);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf_(25.0, bounds.X, 0.05);
    expectf_(25.0, bounds.Y, 0.05);
    expectf_(75.0, bounds.Width, 0.05);
    expectf_(75.0, bounds.Height, 0.05);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, NULL, MetafileFrameUnitMillimeter, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetDC(graphics, &metafile_dc);
    expect(Ok, stat);

    if (stat != Ok)
    {
        GdipDeleteGraphics(graphics);
        GdipDisposeImage((GpImage*)metafile);
        return;
    }

    hbrush = CreateSolidBrush(0xff0000);

    holdbrush = SelectObject(metafile_dc, hbrush);

    Rectangle(metafile_dc, 25, 25, 75, 75);

    SelectObject(metafile_dc, holdbrush);

    DeleteObject(hbrush);

    stat = GdipReleaseDC(graphics, metafile_dc);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf_(25.0, bounds.X, 0.05);
    expectf_(25.0, bounds.Y, 0.05);
    todo_wine expectf_(50.0, bounds.Width, 0.05);
    todo_wine expectf_(50.0, bounds.Height, 0.05);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record pagetransform_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSetPageTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSetPageTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSetPageTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSetPageTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_pagetransform(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    static const GpRectF frame = {0.0, 0.0, 5.0, 5.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpBitmap *bitmap;
    ARGB color;
    GpBrush *brush;
    GpUnit unit;
    REAL scale, dpix, dpiy;
    UINT width, height;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitInch, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &dpix);
    todo_wine expect(InvalidParameter, stat);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &dpiy);
    todo_wine expect(InvalidParameter, stat);

    stat = GdipGetImageWidth((GpImage*)metafile, &width);
    todo_wine expect(InvalidParameter, stat);

    stat = GdipGetImageHeight((GpImage*)metafile, &height);
    todo_wine expect(InvalidParameter, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    /* initial scale */
    stat = GdipGetPageUnit(graphics, &unit);
    expect(Ok, stat);
    expect(UnitDisplay, unit);

    stat = GdipGetPageScale(graphics, &scale);
    expect(Ok, stat);
    expectf(1.0, scale);

    stat = GdipGetDpiX(graphics, &dpix);
    expect(Ok, stat);
    expectf(96.0, dpix);

    stat = GdipGetDpiY(graphics, &dpiy);
    expect(Ok, stat);
    expectf(96.0, dpiy);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 1, 2, 1, 1);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* page unit = pixels */
    stat = GdipSetPageUnit(graphics, UnitPixel);
    expect(Ok, stat);

    stat = GdipGetPageUnit(graphics, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);

    stat = GdipCreateSolidFill((ARGB)0xff00ff00, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 0, 1, 1, 1);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* page scale = 3, unit = pixels */
    stat = GdipSetPageScale(graphics, 3.0);
    expect(Ok, stat);

    stat = GdipGetPageScale(graphics, &scale);
    expect(Ok, stat);
    expectf(3.0, scale);

    stat = GdipCreateSolidFill((ARGB)0xff00ffff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 0, 1, 2, 2);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* page scale = 3, unit = inches */
    stat = GdipSetPageUnit(graphics, UnitInch);
    expect(Ok, stat);

    stat = GdipGetPageUnit(graphics, &unit);
    expect(Ok, stat);
    expect(UnitInch, unit);

    stat = GdipCreateSolidFill((ARGB)0xffff0000, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0/96.0, 0, 1, 1);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* page scale = 3, unit = display */
    stat = GdipSetPageUnit(graphics, UnitDisplay);
    expect(Ok, stat);

    stat = GdipGetPageUnit(graphics, &unit);
    expect(Ok, stat);
    expect(UnitDisplay, unit);

    stat = GdipCreateSolidFill((ARGB)0xffff00ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 3, 3, 2, 2);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, pagetransform_records, "pagetransform metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "pagetransform.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, pagetransform_records, "pagetransform playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 50, 50, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 30, 50, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapGetPixel(bitmap, 10, 30, &color);
    expect(Ok, stat);
    expect(0xff00ff00, color);

    stat = GdipBitmapGetPixel(bitmap, 20, 80, &color);
    expect(Ok, stat);
    expect(0xff00ffff, color);

    stat = GdipBitmapGetPixel(bitmap, 80, 20, &color);
    expect(Ok, stat);
    expect(0xffff0000, color);

    stat = GdipBitmapGetPixel(bitmap, 80, 80, &color);
    expect(Ok, stat);
    expect(0xffff00ff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record worldtransform_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeScaleWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeResetWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeMultiplyWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeRotateWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSetWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeTranslateWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_worldtransform(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    static const GpRectF frame = {0.0, 0.0, 5.0, 5.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpBitmap *bitmap;
    ARGB color;
    GpBrush *brush;
    GpMatrix *transform;
    BOOL identity;
    REAL elements[6];

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipCreateMatrix(&transform);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    /* initial transform */
    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipIsMatrixIdentity(transform, &identity);
    expect(Ok, stat);
    expect(TRUE, identity);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangleI(graphics, brush, 0, 0, 1, 1);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* scale transform */
    stat = GdipScaleWorldTransform(graphics, 2.0, 4.0, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetMatrixElements(transform, elements);
    expect(Ok, stat);
    expectf(2.0, elements[0]);
    expectf(0.0, elements[1]);
    expectf(0.0, elements[2]);
    expectf(4.0, elements[3]);
    expectf(0.0, elements[4]);
    expectf(0.0, elements[5]);

    stat = GdipCreateSolidFill((ARGB)0xff00ff00, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 0.5, 0.5, 0.5, 0.25);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* reset transform */
    stat = GdipResetWorldTransform(graphics);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipIsMatrixIdentity(transform, &identity);
    expect(Ok, stat);
    expect(TRUE, identity);

    stat = GdipCreateSolidFill((ARGB)0xff00ffff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0, 0.0, 1.0, 1.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* multiply transform */
    stat = GdipSetMatrixElements(transform, 2.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    expect(Ok, stat);

    stat = GdipMultiplyWorldTransform(graphics, transform, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetMatrixElements(transform, elements);
    expect(Ok, stat);
    expectf(2.0, elements[0]);
    expectf(0.0, elements[1]);
    expectf(0.0, elements[2]);
    expectf(1.0, elements[3]);
    expectf(0.0, elements[4]);
    expectf(0.0, elements[5]);

    stat = GdipCreateSolidFill((ARGB)0xffff0000, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0, 1.0, 0.5, 1.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* rotate transform */
    stat = GdipRotateWorldTransform(graphics, 90.0, MatrixOrderAppend);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetMatrixElements(transform, elements);
    expect(Ok, stat);
    expectf(0.0, elements[0]);
    expectf(2.0, elements[1]);
    expectf(-1.0, elements[2]);
    expectf(0.0, elements[3]);
    expectf(0.0, elements[4]);
    expectf(0.0, elements[5]);

    stat = GdipCreateSolidFill((ARGB)0xffff00ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0, -1.0, 0.5, 1.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* set transform */
    stat = GdipSetMatrixElements(transform, 1.0, 0.0, 0.0, 3.0, 0.0, 0.0);
    expect(Ok, stat);

    stat = GdipSetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetMatrixElements(transform, elements);
    expect(Ok, stat);
    expectf(1.0, elements[0]);
    expectf(0.0, elements[1]);
    expectf(0.0, elements[2]);
    expectf(3.0, elements[3]);
    expectf(0.0, elements[4]);
    expectf(0.0, elements[5]);

    stat = GdipCreateSolidFill((ARGB)0xffffff00, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0, 1.0, 1.0, 1.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* translate transform */
    stat = GdipTranslateWorldTransform(graphics, -1.0, 0.0, MatrixOrderAppend);
    expect(Ok, stat);

    stat = GdipGetWorldTransform(graphics, transform);
    expect(Ok, stat);

    stat = GdipGetMatrixElements(transform, elements);
    expect(Ok, stat);
    expectf(1.0, elements[0]);
    expectf(0.0, elements[1]);
    expectf(0.0, elements[2]);
    expectf(3.0, elements[3]);
    expectf(-1.0, elements[4]);
    expectf(0.0, elements[5]);

    stat = GdipCreateSolidFill((ARGB)0xffffffff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 1.0, 1.0, 1.0, 1.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipDeleteMatrix(transform);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, worldtransform_records, "worldtransform metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "worldtransform.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, worldtransform_records, "worldtransform playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 80, 80, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 10, 10, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapGetPixel(bitmap, 30, 50, &color);
    expect(Ok, stat);
    expect(0xff00ff00, color);

    stat = GdipBitmapGetPixel(bitmap, 30, 10, &color);
    expect(Ok, stat);
    expect(0xff00ffff, color);

    stat = GdipBitmapGetPixel(bitmap, 50, 30, &color);
    expect(Ok, stat);
    expect(0xffff0000, color);

    stat = GdipBitmapGetPixel(bitmap, 10, 50, &color);
    expect(Ok, stat);
    expect(0xffff00ff, color);

    stat = GdipBitmapGetPixel(bitmap, 30, 90, &color);
    expect(Ok, stat);
    expect(0xffffff00, color);

    stat = GdipBitmapGetPixel(bitmap, 10, 90, &color);
    expect(Ok, stat);
    expect(0xffffffff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static void test_converttoemfplus(void)
{
    GpStatus (WINAPI *pGdipConvertToEmfPlus)( const GpGraphics *graphics, GpMetafile *metafile, BOOL *succ,
              EmfType emfType, const WCHAR *description, GpMetafile **outmetafile);
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpStatus stat;
    GpMetafile *metafile, *metafile2 = NULL, *emhmeta;
    GpGraphics *graphics;
    HDC hdc;
    BOOL succ;
    HMODULE mod = GetModuleHandleA("gdiplus.dll");

    pGdipConvertToEmfPlus = (void*)GetProcAddress( mod, "GdipConvertToEmfPlus");
    if(!pGdipConvertToEmfPlus)
    {
        /* GdipConvertToEmfPlus was introduced in Windows Vista. */
        win_skip("GDIPlus version 1.1 not available\n");
        return;
    }

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, MetafileTypeEmf, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &emhmeta);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    /* Invalid Parameters */
    stat = pGdipConvertToEmfPlus(NULL, metafile, &succ, EmfTypeEmfPlusOnly, description, &metafile2);
    expect(InvalidParameter, stat);

    stat = pGdipConvertToEmfPlus(graphics, NULL, &succ, EmfTypeEmfPlusOnly, description, &metafile2);
    expect(InvalidParameter, stat);

    stat = pGdipConvertToEmfPlus(graphics, metafile, &succ, EmfTypeEmfPlusOnly, description, NULL);
    expect(InvalidParameter, stat);

    stat = pGdipConvertToEmfPlus(graphics, metafile, NULL, MetafileTypeInvalid, NULL, &metafile2);
    expect(InvalidParameter, stat);

    stat = pGdipConvertToEmfPlus(graphics, metafile, NULL, MetafileTypeEmfPlusDual+1, NULL, &metafile2);
    expect(InvalidParameter, stat);

    /* If we are already an Enhanced Metafile then the conversion fails. */
    stat = pGdipConvertToEmfPlus(graphics, emhmeta, NULL, EmfTypeEmfPlusOnly, NULL, &metafile2);
    todo_wine expect(InvalidParameter, stat);

    stat = pGdipConvertToEmfPlus(graphics, metafile, NULL, EmfTypeEmfPlusOnly, NULL, &metafile2);
    todo_wine expect(Ok, stat);
    if(metafile2)
        GdipDisposeImage((GpImage*)metafile2);

    succ = FALSE;
    stat = pGdipConvertToEmfPlus(graphics, metafile, &succ, EmfTypeEmfPlusOnly, NULL, &metafile2);
    todo_wine expect(Ok, stat);
    if(metafile2)
        GdipDisposeImage((GpImage*)metafile2);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)emhmeta);
    expect(Ok, stat);
}

static void test_frameunit(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc;
    static const GpRectF frame = {0.0, 0.0, 5.0, 5.0};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GpUnit unit;
    REAL dpix, dpiy;
    GpRectF bounds;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitInch, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    ok(bounds.Width == 1.0 || broken(bounds.Width == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Width);
    ok(bounds.Height == 1.0 || broken(bounds.Height == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Height);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    ok(bounds.Width == 1.0 || broken(bounds.Width == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Width);
    ok(bounds.Height == 1.0 || broken(bounds.Height == 0.0) /* xp sp1 */,
        "expected 1.0, got %f\n", bounds.Height);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipGetImageHorizontalResolution((GpImage*)metafile, &dpix);
    expect(Ok, stat);

    stat = GdipGetImageVerticalResolution((GpImage*)metafile, &dpiy);
    expect(Ok, stat);

    stat = GdipGetImageBounds((GpImage*)metafile, &bounds, &unit);
    expect(Ok, stat);
    expect(UnitPixel, unit);
    expectf(0.0, bounds.X);
    expectf(0.0, bounds.Y);
    expectf_(5.0 * dpix, bounds.Width, 1.0);
    expectf_(5.0 * dpiy, bounds.Height, 1.0);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record container_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeBeginContainerNoParams},
    {0, EmfPlusRecordTypeScaleWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndContainer},
    {0, EmfPlusRecordTypeScaleWorldTransform},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeSave},
    {0, EmfPlusRecordTypeRestore},
    {0, EmfPlusRecordTypeScaleWorldTransform},
    {0, EmfPlusRecordTypeBeginContainerNoParams},
    {0, EmfPlusRecordTypeScaleWorldTransform},
    {0, EmfPlusRecordTypeBeginContainerNoParams},
    {0, EmfPlusRecordTypeEndContainer},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeBeginContainer},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndContainer},
    {0, EmfPlusRecordTypeBeginContainerNoParams},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_containers(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    GpBitmap *bitmap;
    GpBrush *brush;
    ARGB color;
    HDC hdc;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GraphicsContainer state1, state2;
    GpRectF srcrect, dstrect;
    REAL dpix, dpiy;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    /* Normal usage */
    stat = GdipBeginContainer2(graphics, &state1);
    expect(Ok, stat);

    stat = GdipScaleWorldTransform(graphics, 2.0, 2.0, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff000000, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 5.0, 5.0, 5.0, 5.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipEndContainer(graphics, state1);
    expect(Ok, stat);

    stat = GdipScaleWorldTransform(graphics, 1.0, 1.0, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 5.0, 5.0, 5.0, 5.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipSaveGraphics(graphics, &state1);
    expect(Ok, stat);

    stat = GdipRestoreGraphics(graphics, state1);
    expect(Ok, stat);

    /* Popping two states at once */
    stat = GdipScaleWorldTransform(graphics, 2.0, 2.0, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipBeginContainer2(graphics, &state1);
    expect(Ok, stat);

    stat = GdipScaleWorldTransform(graphics, 4.0, 4.0, MatrixOrderPrepend);
    expect(Ok, stat);

    stat = GdipBeginContainer2(graphics, &state2);
    expect(Ok, stat);

    stat = GdipEndContainer(graphics, state1);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff00ff00, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 20.0, 20.0, 5.0, 5.0);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    /* With transform applied */
    stat = GdipGetDpiX(graphics, &dpix);
    expect(Ok, stat);

    stat = GdipGetDpiY(graphics, &dpiy);
    expect(Ok, stat);

    srcrect.X = 0.0;
    srcrect.Y = 0.0;
    srcrect.Width = 1.0;
    srcrect.Height = 1.0;

    dstrect.X = 25.0;
    dstrect.Y = 0.0;
    dstrect.Width = 5.0;
    dstrect.Height = 5.0;

    stat = GdipBeginContainer(graphics, &dstrect, &srcrect, UnitInch, &state1);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff00ffff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 0.0, 0.0, dpix, dpiy);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipEndContainer(graphics, state1);
    expect(Ok, stat);

    /* Restoring an invalid state seems to break the graphics object? */
    if (0) {
        stat = GdipEndContainer(graphics, state1);
        expect(Ok, stat);
    }

    /* Ending metafile with a state open */
    stat = GdipBeginContainer2(graphics, &state1);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, container_records, "container metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "container.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, container_records, "container playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 80, 80, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 12, 12, &color);
    expect(Ok, stat);
    expect(0xff000000, color);

    stat = GdipBitmapGetPixel(bitmap, 8, 8, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipBitmapGetPixel(bitmap, 42, 42, &color);
    expect(Ok, stat);
    expect(0xff00ff00, color);

    stat = GdipBitmapGetPixel(bitmap, 55, 5, &color);
    expect(Ok, stat);
    expect(0xff00ffff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record clipping_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeSave},
    {0, EmfPlusRecordTypeSetClipRect},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeRestore},
    {0, EmfPlusRecordTypeSetClipRect},
    {0, EmfPlusRecordTypeFillRects},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_clipping(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    GpBitmap *bitmap;
    GpBrush *brush;
    GpRectF rect;
    ARGB color;
    HDC hdc;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{100.0,0.0},{0.0,100.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    GraphicsState state;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
        return;

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipSaveGraphics(graphics, &state);
    expect(Ok, stat);

    stat = GdipGetVisibleClipBounds(graphics, &rect);
    expect(Ok, stat);
    ok(rect.X == -0x400000, "rect.X = %f\n", rect.X);
    ok(rect.Y == -0x400000, "rect.Y = %f\n", rect.Y);
    ok(rect.Width == 0x800000, "rect.Width = %f\n", rect.Width);
    ok(rect.Height == 0x800000, "rect.Height = %f\n", rect.Height);

    stat = GdipSetClipRect(graphics, 30, 30, 10, 10, CombineModeReplace);
    expect(Ok, stat);

    stat = GdipGetVisibleClipBounds(graphics, &rect);
    expect(Ok, stat);
    ok(rect.X == 30, "rect.X = %f\n", rect.X);
    ok(rect.Y == 30, "rect.Y = %f\n", rect.Y);
    ok(rect.Width == 10, "rect.Width = %f\n", rect.Width);
    ok(rect.Height == 10, "rect.Height = %f\n", rect.Height);

    stat = GdipCreateSolidFill((ARGB)0xff000000, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 0, 0, 100, 100);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipRestoreGraphics(graphics, state);
    expect(Ok, stat);

    stat = GdipSetClipRect(graphics, 30, 30, 10, 10, CombineModeXor);
    expect(Ok, stat);

    stat = GdipCreateSolidFill((ARGB)0xff0000ff, (GpSolidFill**)&brush);
    expect(Ok, stat);

    stat = GdipFillRectangle(graphics, brush, 30, 30, 20, 10);
    expect(Ok, stat);

    stat = GdipDeleteBrush(brush);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, clipping_records, "clipping metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "clipping.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, clipping_records, "clipping playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 80, 80, &color);
    expect(Ok, stat);
    expect(0, color);

    stat = GdipBitmapGetPixel(bitmap, 35, 35, &color);
    expect(Ok, stat);
    expect(0xff000000, color);

    stat = GdipBitmapGetPixel(bitmap, 45, 35, &color);
    expect(Ok, stat);
    expect(0xff0000ff, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static void test_gditransform_cb(GpMetafile* metafile, EmfPlusRecordType record_type,
    unsigned int flags, unsigned int dataSize, const unsigned char *pStr)
{
    static const XFORM xform = {0.5, 0, 0, 0.5, 0, 0};
    static const RECTL rectangle = {0,0,100,100};
    GpStatus stat;

    stat = GdipPlayMetafileRecord(metafile, EMR_SETWORLDTRANSFORM, 0, sizeof(xform), (void*)&xform);
    expect(Ok, stat);

    stat = GdipPlayMetafileRecord(metafile, EMR_RECTANGLE, 0, sizeof(rectangle), (void*)&rectangle);
    expect(Ok, stat);
}

static const emfplus_record gditransform_records[] = {
    {0, EMR_HEADER},
    {0, EMR_CREATEBRUSHINDIRECT},
    {0, EMR_SELECTOBJECT},
    {0, EMR_GDICOMMENT, 0, test_gditransform_cb},
    {0, EMR_SELECTOBJECT},
    {0, EMR_DELETEOBJECT},
    {0, EMR_EOF},
    {0}
};

static void test_gditransform(void)
{
    GpStatus stat;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HDC hdc, metafile_dc;
    HENHMETAFILE hemf;
    MetafileHeader header;
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    static const GpPointF dst_points[3] = {{0.0,0.0},{40.0,0.0},{0.0,40.0}};
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    HBRUSH hbrush, holdbrush;
    GpBitmap *bitmap;
    ARGB color;

    hdc = CreateCompatibleDC(0);

    stat = GdipRecordMetafile(hdc, EmfTypeEmfOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    DeleteDC(hdc);

    if (stat != Ok)
            return;

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(InvalidParameter, stat);

    memset(&header, 0xaa, sizeof(header));
    stat = GdipGetMetafileHeaderFromMetafile(metafile, &header);
    expect(Ok, stat);
    expect(MetafileTypeEmf, header.Type);
    ok(header.Version == 0xdbc01001 || header.Version == 0xdbc01002, "Unexpected version %x\n", header.Version);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipGetDC(graphics, &metafile_dc);
    expect(Ok, stat);

    if (stat != Ok)
    {
        GdipDeleteGraphics(graphics);
        GdipDisposeImage((GpImage*)metafile);
        return;
    }

    hbrush = CreateSolidBrush(0xff);

    holdbrush = SelectObject(metafile_dc, hbrush);

    GdiComment(metafile_dc, 8, (const BYTE*)"winetest");

    SelectObject(metafile_dc, holdbrush);

    DeleteObject(hbrush);

    stat = GdipReleaseDC(graphics, metafile_dc);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    check_metafile(metafile, gditransform_records, "gditransform metafile", dst_points, &frame, UnitPixel);

    sync_metafile(&metafile, "gditransform.emf");

    stat = GdipCreateBitmapFromScan0(100, 100, 0, PixelFormat32bppARGB, NULL, &bitmap);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)bitmap, &graphics);
    expect(Ok, stat);

    play_metafile(metafile, graphics, gditransform_records, "gditransform playback", dst_points, &frame, UnitPixel);

    stat = GdipBitmapGetPixel(bitmap, 10, 10, &color);
    expect(Ok, stat);
    expect(0xffff0000, color);

    stat = GdipBitmapGetPixel(bitmap, 30, 30, &color);
    expect(Ok, stat);
    expect(0x00000000, color);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)bitmap);
    expect(Ok, stat);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record draw_image_bitmap_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeDrawImagePoints},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static const emfplus_record draw_image_metafile_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeObject},
    /* metafile object */
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeDrawImagePoints},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    /* end of metafile object */
    {0, EmfPlusRecordTypeDrawImagePoints},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_drawimage(void)
{
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    static const GpPointF dst_points[3] = {{10.0,10.0},{85.0,15.0},{10.0,80.0}};
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};
    const ColorMatrix double_red = {{
        {2.0,0.0,0.0,0.0,0.0},
        {0.0,1.0,0.0,0.0,0.0},
        {0.0,0.0,1.0,0.0,0.0},
        {0.0,0.0,0.0,1.0,0.0},
        {0.0,0.0,0.0,0.0,1.0}}};

    GpImageAttributes *imageattr;
    GpMetafile *metafile;
    GpGraphics *graphics;
    HENHMETAFILE hemf;
    GpStatus stat;
    BITMAPINFO info;
    BYTE buff[400];
    GpImage *image;
    HDC hdc;

    hdc = CreateCompatibleDC(0);
    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 10;
    info.bmiHeader.biHeight = 10;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    memset(buff, 0x80, sizeof(buff));
    stat = GdipCreateBitmapFromGdiDib(&info, buff, (GpBitmap**)&image);
    expect(Ok, stat);

    stat = GdipCreateImageAttributes(&imageattr);
    expect(Ok, stat);

    stat = GdipSetImageAttributesColorMatrix(imageattr, ColorAdjustTypeDefault,
            TRUE, &double_red, NULL, ColorMatrixFlagsDefault);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, image, dst_points, 3,
            0.0, 0.0, 10.0, 10.0, UnitPixel, imageattr, NULL, NULL);
    GdipDisposeImageAttributes(imageattr);
    expect(Ok, stat);

    GdipDisposeImage(image);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);
    sync_metafile(&metafile, "draw_image_bitmap.emf");

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    check_emfplus(hemf, draw_image_bitmap_records, "draw image bitmap");

    /* test drawing metafile */
    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreateMetafileFromEmf(hemf, TRUE, (GpMetafile**)&image);
    expect(Ok, stat);

    stat = GdipDrawImagePointsRect(graphics, image, dst_points, 3,
            0.0, 0.0, 100.0, 100.0, UnitPixel, NULL, NULL, NULL);
    expect(Ok, stat);

    GdipDisposeImage(image);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);
    sync_metafile(&metafile, "draw_image_metafile.emf");

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    if (GetProcAddress(GetModuleHandleA("gdiplus.dll"), "GdipConvertToEmfPlus"))
    {
        check_emfplus(hemf, draw_image_metafile_records, "draw image metafile");
    }
    else
    {
        win_skip("draw image metafile records tests skipped\n");
    }
    DeleteEnhMetaFile(hemf);

    DeleteDC(hdc);
    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record properties_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeSetTextRenderingHint},
    {0, EmfPlusRecordTypeSetPixelOffsetMode},
    {0, EmfPlusRecordTypeSetAntiAliasMode},
    {0, EmfPlusRecordTypeSetCompositingMode},
    {0, EmfPlusRecordTypeSetCompositingQuality},
    {0, EmfPlusRecordTypeSetInterpolationMode},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_properties(void)
{
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};

    GpMetafile *metafile;
    GpGraphics *graphics;
    HENHMETAFILE hemf;
    GpStatus stat;
    HDC hdc;

    hdc = CreateCompatibleDC(0);
    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);
    DeleteDC(hdc);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipSetTextRenderingHint(graphics, TextRenderingHintSystemDefault);
    expect(Ok, stat);
    stat = GdipSetTextRenderingHint(graphics, TextRenderingHintAntiAlias);
    expect(Ok, stat);

    stat = GdipSetPixelOffsetMode(graphics, PixelOffsetModeHighQuality);
    expect(Ok, stat);
    stat = GdipSetPixelOffsetMode(graphics, PixelOffsetModeHighQuality);
    expect(Ok, stat);

    stat = GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    expect(Ok, stat);
    stat = GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    expect(Ok, stat);

    stat = GdipSetCompositingMode(graphics, CompositingModeSourceOver);
    expect(Ok, stat);
    stat = GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
    expect(Ok, stat);

    stat = GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);
    expect(Ok, stat);
    stat = GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);
    expect(Ok, stat);

    stat = GdipSetInterpolationMode(graphics, InterpolationModeDefault);
    expect(Ok, stat);
    stat = GdipSetInterpolationMode(graphics, InterpolationModeHighQuality);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);
    sync_metafile(&metafile, "properties.emf");

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    check_emfplus(hemf, properties_records, "properties");
    DeleteEnhMetaFile(hemf);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record draw_path_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeDrawPath},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_drawpath(void)
{
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};

    GpMetafile *metafile;
    GpGraphics *graphics;
    HENHMETAFILE hemf;
    GpStatus stat;
    GpPath *path;
    GpPen *pen;
    HDC hdc;

    hdc = CreateCompatibleDC(0);
    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);
    DeleteDC(hdc);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreatePath(FillModeAlternate, &path);
    expect(Ok, stat);
    stat = GdipAddPathLine(path, 5, 5, 30, 30);
    expect(Ok, stat);

    stat = GdipCreatePen1((ARGB)0xffff00ff, 10.0f, UnitPixel, &pen);
    expect(Ok, stat);

    stat = GdipDrawPath(graphics, pen, path);
    expect(Ok, stat);

    stat = GdipDeletePen(pen);
    expect(Ok, stat);
    stat = GdipDeletePath(path);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);
    sync_metafile(&metafile, "draw_path.emf");

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    check_emfplus(hemf, draw_path_records, "draw path");
    DeleteEnhMetaFile(hemf);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

static const emfplus_record fill_path_records[] = {
    {0, EMR_HEADER},
    {0, EmfPlusRecordTypeHeader},
    {0, EmfPlusRecordTypeObject},
    {0, EmfPlusRecordTypeFillPath},
    {1, EMR_SAVEDC},
    {1, EMR_SETICMMODE},
    {1, EMR_BITBLT},
    {1, EMR_RESTOREDC},
    {0, EmfPlusRecordTypeEndOfFile},
    {0, EMR_EOF},
    {0}
};

static void test_fillpath(void)
{
    static const WCHAR description[] = {'w','i','n','e','t','e','s','t',0};
    static const GpRectF frame = {0.0, 0.0, 100.0, 100.0};

    GpMetafile *metafile;
    GpGraphics *graphics;
    GpSolidFill *brush;
    HENHMETAFILE hemf;
    GpStatus stat;
    GpPath *path;
    HDC hdc;

    hdc = CreateCompatibleDC(0);
    stat = GdipRecordMetafile(hdc, EmfTypeEmfPlusOnly, &frame, MetafileFrameUnitPixel, description, &metafile);
    expect(Ok, stat);
    DeleteDC(hdc);

    stat = GdipGetImageGraphicsContext((GpImage*)metafile, &graphics);
    expect(Ok, stat);

    stat = GdipCreatePath(FillModeAlternate, &path);
    expect(Ok, stat);
    stat = GdipAddPathLine(path, 5, 5, 30, 30);
    expect(Ok, stat);
    stat = GdipAddPathLine(path, 30, 30, 5, 30);
    expect(Ok, stat);

    stat = GdipCreateSolidFill(0xffaabbcc, &brush);
    expect(Ok, stat);

    stat = GdipFillPath(graphics, (GpBrush*)brush, path);
    expect(Ok, stat);

    stat = GdipDeleteBrush((GpBrush*)brush);
    expect(Ok, stat);
    stat = GdipDeletePath(path);
    expect(Ok, stat);

    stat = GdipDeleteGraphics(graphics);
    expect(Ok, stat);
    sync_metafile(&metafile, "fill_path.emf");

    stat = GdipGetHemfFromMetafile(metafile, &hemf);
    expect(Ok, stat);

    check_emfplus(hemf, fill_path_records, "fill path");
    DeleteEnhMetaFile(hemf);

    stat = GdipDisposeImage((GpImage*)metafile);
    expect(Ok, stat);
}

START_TEST(metafile)
{
    struct GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    int myARGC;
    char **myARGV;
    HMODULE hmsvcrt;
    int (CDECL * _controlfp_s)(unsigned int *cur, unsigned int newval, unsigned int mask);

    /* Enable all FP exceptions except _EM_INEXACT, which gdi32 can trigger */
    hmsvcrt = LoadLibraryA("msvcrt");
    _controlfp_s = (void*)GetProcAddress(hmsvcrt, "_controlfp_s");
    if (_controlfp_s) _controlfp_s(0, 0, 0x0008001e);

    gdiplusStartupInput.GdiplusVersion              = 1;
    gdiplusStartupInput.DebugEventCallback          = NULL;
    gdiplusStartupInput.SuppressBackgroundThread    = 0;
    gdiplusStartupInput.SuppressExternalCodecs      = 0;

    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    myARGC = winetest_get_mainargs( &myARGV );

    if (myARGC >= 3)
    {
        if (!strcmp(myARGV[2], "save"))
            save_metafiles = TRUE;
        else if (!strcmp(myARGV[2], "load"))
            load_metafiles = TRUE;
    }

    test_empty();
    test_getdc();
    test_emfonly();
    test_fillrect();
    test_clear();
    test_nullframerect();
    test_pagetransform();
    test_worldtransform();
    test_converttoemfplus();
    test_frameunit();
    test_containers();
    test_clipping();
    test_gditransform();
    test_drawimage();
    test_properties();
    test_drawpath();
    test_fillpath();

    GdiplusShutdown(gdiplusToken);
}
