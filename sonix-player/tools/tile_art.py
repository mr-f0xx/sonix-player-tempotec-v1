"""Draw the stacked-card artwork used by the home grids' tiles.

Each picture is two rounded squares of the same hue -- a lighter back plate laid
down at -13 degrees and a front card at +7 -- with the card's function drawn as
a white line glyph on top. The result is a 140x140 PNG that
tools/svg_to_lvgl.py then emits at the size the cards draw it, so the player
never resamples one.

    python3 tools/tile_art.py

writes the four Subsonic pictures into assets/icons. The SoundCloud home page's
four (sc-likes, sc-reposts, sc-playlists, sc-account) were drawn with this same
recipe and are kept as they are.

Needs cairosvg (for the glyph SVGs) and Pillow, like the icon generator.
"""

import io, os, sys

import cairosvg
from PIL import Image, ImageDraw, ImageFilter

S = 4           # supersample factor
CANVAS = 140
CARD = 96
RADIUS = 26
GLYPH = 56
ART_BOX = (124, 116)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def rounded_mask(size, radius):
    img = Image.new('L', (size, size), 0)
    ImageDraw.Draw(img).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return img


def card(top, bottom, sheen=38):
    """A rounded square filled with a vertical gradient, white keyline + sheen."""
    size = CARD * S
    g = Image.new('RGBA', (size, size))
    d = ImageDraw.Draw(g)
    for y in range(size):
        d.line([(0, y), (size, y)], fill=lerp(top, bottom, y / (size - 1)) + (255,))
    g.putalpha(rounded_mask(size, RADIUS * S))

    # Top sheen: a soft white wash across the upper half, like light falling
    # on the card.
    sheen_layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sheen_layer)
    sd.ellipse([-size * 0.35, -size * 0.55, size * 1.35, size * 0.55], fill=(255, 255, 255, sheen))
    sheen_layer = sheen_layer.filter(ImageFilter.GaussianBlur(size * 0.06))
    sheen_layer.putalpha(Image.composite(sheen_layer.getchannel('A'),
                                         Image.new('L', (size, size), 0),
                                         rounded_mask(size, RADIUS * S)))
    g.alpha_composite(sheen_layer)

    # Keyline, 1.4 px at final size.
    d = ImageDraw.Draw(g)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=RADIUS * S,
                        outline=(255, 255, 255, 78), width=max(1, int(round(1.4 * S))))
    return g


def glyph(path, size):
    png = cairosvg.svg2png(url=path, output_width=size, output_height=size)
    img = Image.open(io.BytesIO(png)).convert('RGBA')
    white = Image.new('RGBA', img.size, (255, 255, 255, 255))
    white.putalpha(img.getchannel('A'))
    return white


def build(main, glyph_path, out):
    # Back plate: the same hue, lighter, so the stack reads as two surfaces.
    back = card(lerp(main, (255, 255, 255), 0.30), lerp(main, (255, 255, 255), 0.10), sheen=14)
    back = back.rotate(-13, resample=Image.BICUBIC, expand=True)

    front = card(lerp(main, (255, 255, 255), 0.22), main)
    front = front.rotate(7, resample=Image.BICUBIC, expand=True)

    canvas = Image.new('RGBA', (CANVAS * S, CANVAS * S), (0, 0, 0, 0))
    canvas.alpha_composite(back, (58 * S - back.width // 2, 62 * S - back.height // 2))
    canvas.alpha_composite(front, (84 * S - front.width // 2, 84 * S - front.height // 2))

    g = glyph(glyph_path, GLYPH * S)
    canvas.alpha_composite(g, (84 * S - g.width // 2, 84 * S - g.height // 2))

    art = canvas.resize((CANVAS, CANVAS), Image.LANCZOS)

    # Fit the drawn shape into the box the cards give it, centred.
    box = art.getbbox()
    crop = art.crop(box)
    scale = min(ART_BOX[0] / crop.width, ART_BOX[1] / crop.height)
    crop = crop.resize((max(1, round(crop.width * scale)), max(1, round(crop.height * scale))), Image.LANCZOS)
    out_img = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    out_img.alpha_composite(crop, ((CANVAS - crop.width) // 2, (CANVAS - crop.height) // 2))
    out_img.save(out)
    print('wrote', out, out_img.size)


if __name__ == '__main__':
    icons = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'icons')
    tiles = [
        ('sub-server.png', (56, 119, 213), 'music-settings.svg'),
        ('sub-test.png', (46, 163, 104), 'wifi-high.svg'),
        ('sub-folders.png', (34, 164, 190), 'folder.svg'),
        ('sub-artists.png', (113, 77, 217), 'artist.svg'),
    ]
    for name, colour, svg in tiles:
        build(colour, os.path.join(icons, svg), os.path.join(icons, name))
