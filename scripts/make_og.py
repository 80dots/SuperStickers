# SNS 공유 미리보기 이미지(web/assets/img/og.png, 1200x630)를 만든다.
#
#   python scripts/make_og.py
#
# 랜딩 사이트와 같은 팔레트를 쓰며, 사이트를 다시 디자인하면 여기 색·문구도 함께 고친다.
# 폰트는 Windows 기본 맑은 고딕을 쓴다(한글 포함).

import os
from PIL import Image, ImageDraw, ImageFont

W, H = 1200, 630
CREAM      = (248, 243, 233)
CREAM_DEEP = (241, 233, 218)
INK        = (27, 38, 32)
INK_SOFT   = (85, 100, 90)
FOREST     = (30, 58, 44)
YELLOW     = (255, 244, 184)
MINT       = (200, 240, 220)
BLUE       = (207, 229, 255)
HL_YELLOW  = (255, 230, 138)

FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
BOLD = os.path.join(FONT_DIR, "malgunbd.ttf")
REG  = os.path.join(FONT_DIR, "malgun.ttf")

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(root, "web", "assets", "img", "og.png")


def font(path, size):
    return ImageFont.truetype(path, size)


def sticker(size, fill, rotate, lines):
    """접힌 모서리가 있는 스티커 한 장을 그려 회전한 RGBA 이미지로 돌려준다."""
    w, h = size
    pad = 60
    layer = Image.new("RGBA", (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    box = (pad, pad, pad + w, pad + h)
    d.rounded_rectangle(box, radius=26, fill=fill + (255,))

    # 접힌 모서리
    fold = 46
    d.polygon(
        [(pad + w - fold, pad + h), (pad + w, pad + h - fold), (pad + w, pad + h)],
        fill=(0, 0, 0, 26),
    )

    # 글줄
    y = pad + 34
    for frac in lines:
        d.rounded_rectangle(
            (pad + 30, y, pad + 30 + int((w - 60) * frac), y + 13), radius=7,
            fill=INK + (40,),
        )
        y += 30
    return layer.rotate(rotate, resample=Image.BICUBIC, expand=True)


def main():
    img = Image.new("RGB", (W, H), CREAM)
    d = ImageDraw.Draw(img)

    # 오른쪽 위 은은한 색 덩어리
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((W - 420, -220, W + 160, 360), fill=MINT + (150,))
    gd.ellipse((-260, H - 210, 180, H + 240), fill=YELLOW + (110,))
    img.paste(Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB"), (0, 0))
    d = ImageDraw.Draw(img)

    # 스티커 세 장
    img.paste(s := sticker((250, 250), BLUE, 7, [.9, .7, .45]), (760, 60), s)
    img.paste(s := sticker((250, 250), MINT, -6, [.8, .55, .8]), (900, 300), s)
    img.paste(s := sticker((300, 300), YELLOW, -3, [.95, .8, .6, .35]), (690, 230), s)

    # 워드마크 — 로고(뒤쪽 민트 + 접힌 모서리가 있는 앞쪽 노랑 스티커)와 같은 구성
    d.rounded_rectangle((68, 86, 110, 128), radius=10, fill=MINT)
    d.rounded_rectangle((84, 74, 132, 122), radius=12, fill=YELLOW)
    d.polygon([(116, 122), (132, 106), (132, 122)], fill=(235, 210, 119))
    for i, w in enumerate((30, 30, 17)):
        y = 86 + i * 11
        d.rounded_rectangle((93, y, 93 + w, y + 4), radius=2, fill=(INK[0], INK[1], INK[2]))
    d.text((152, 76), "Super Stickers", font=font(BOLD, 34), fill=INK)

    # 제목 (형광펜 밑줄 한 줄 포함)
    f_title = font(BOLD, 62)
    d.text((72, 210), "바탕화면에 붙이는", font=f_title, fill=INK)
    line2_y = 292
    hl_w = d.textlength("똑똑한", font=f_title)
    d.rounded_rectangle((66, line2_y + 46, 66 + hl_w + 14, line2_y + 74), radius=4, fill=HL_YELLOW)
    d.text((72, line2_y), "똑똑한", font=f_title, fill=INK)
    d.text((72 + hl_w + 18, line2_y), "스티커 메모", font=f_title, fill=INK)

    d.text((72, 396), "Smart sticky notes for your Windows desktop",
           font=font(REG, 27), fill=INK_SOFT)

    # 아래 칩
    chips = ["무료 · 오픈소스", "형광펜 · 선택 메뉴", "로컬 AI", "데이터는 내 PC에만"]
    x = 72
    f_chip = font(BOLD, 21)
    for i, text in enumerate(chips):
        tw = d.textlength(text, font=f_chip)
        w = int(tw) + 40
        fill = FOREST if i == 0 else CREAM_DEEP
        color = CREAM if i == 0 else INK_SOFT
        d.rounded_rectangle((x, 480, x + w, 530), radius=25, fill=fill)
        d.text((x + 20, 490), text, font=f_chip, fill=color)
        x += w + 12

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT, optimize=True)
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
