# SwitchDeck

<p align="center">
  <strong>Turn your Nintendo Switch into a Macro Deck control panel.</strong><br>
  Nintendo Switch’inizi Macro Deck kontrol paneline dönüştürün.
</p>

<p align="center">
  <a href="#english">English</a> ·
  <a href="#türkçe">Türkçe</a> ·
  <a href="#screenshots--ekran-görüntüleri">Screenshots / Ekran görüntüleri</a> ·
  <a href="https://github.com/sertay1/switchdeck/releases">Releases / Sürümler</a>
</p>

![SwitchDeck running on a Nintendo Switch OLED on a gaming desk](docs/images/switchdeck-setup.jpg)

## English

SwitchDeck is a Nintendo Switch homebrew app that shows your Macro Deck profiles and widgets on the console. Set up your pages and actions in Macro Deck on your PC, then use the touchscreen or Joy-Con buttons to control them from the Switch.

### Features

- **Your deck on the Switch.** Browse Macro Deck profiles and folders. Use the button labels, layouts, and styles configured on your PC.
- **Controls and live widgets.** Run macros, adjust volume, control media playback, and view album art, weather, clocks, timers, and CPU, GPU, and RAM gauges. Widget availability depends on your Macro Deck setup.
- **Themes and idle display.** Choose Macro Deck, Light, or OLED Pure Black. Configure the status bar and choose whether an idle console dims, shows a black screen, or enters sleep mode.
- **Local pairing.** Find Macro Deck on your local network, pair with a six-digit code, and reconnect with the saved session.

### Requirements

- A Nintendo Switch running custom firmware (CFW) with access to the Homebrew Menu.
- A PC running [Macro Deck 3](https://macro-deck.app/) and a profile with at least one widget or action.
- The Switch and PC on the same local network.

### Installation

1. Download the **SD ZIP** from [GitHub Releases](https://github.com/sertay1/switchdeck/releases). Copy the included `switch` folder to the root of your SD card. If you are using the repository copy, the same layout is under `SD_Karta_Atilacak`.
2. Open **SwitchDeck** from the Homebrew Menu. The executable should be at `sdmc:/switch/SwitchDeck/SwitchDeck.nro`.
3. Open **Settings → Connection & Server**. Select **Search Network**, or enter the PC’s local IP address and the server port manually. Macro Deck 3 uses port `8193` by default.
4. On the PC, open **Devices → Add Device** in Macro Deck and generate a six-digit pairing code. Enter it on the Switch and select **Connect Now**.

SwitchDeck saves the pairing on the SD card. Pair again if you change the server address or port. For a legacy Macro Deck 2 server, enter its port and leave the pairing code blank.

### Controls

| Input | Action |
| --- | --- |
| Touchscreen | Use widgets. Tap, double-tap, and hold actions depend on the widget. |
| D-pad / left stick | Move focus. |
| A | Activate the focused widget. |
| B | Go to the parent folder or close Settings. |
| L / R | Switch pages on the deck or tabs in Settings. |
| ZL / ZR | Decrease or increase the focused slider. |
| Y | Reload the deck or reconnect if disconnected. |
| X / − | Open Settings. Press − again to close it. |
| + | Exit SwitchDeck. |

### Build from source

Install devkitPro, devkitA64, libnx, and the Switch portlibs listed in the [Makefile](Makefile). Run `make` in the devkitPro shell to build `SwitchDeck.nro`. The build embeds [the CA certificate bundle](romfs/tls-ca-bundle.pem) for TLS verification of GitHub requests. See [regression test instructions](tests/README.txt).

[Back to top](#switchdeck) · [Türkçe](#türkçe)

## Türkçe

SwitchDeck, Macro Deck profillerinizi ve widget’larınızı Nintendo Switch ekranına taşıyan bir homebrew uygulamasıdır. Sayfaları ve eylemleri bilgisayarda Macro Deck üzerinden düzenleyip Switch’in dokunmatik ekranı veya Joy-Con tuşlarıyla kullanabilirsiniz.

### Özellikler

- **Macro Deck profilleri Switch’te.** Profiller ve klasörler arasında dolaşın. Bilgisayarda ayarladığınız düğme etiketlerini, yerleşimleri ve stilleri kullanın.
- **Makrolar ve canlı widget’lar.** Makro çalıştırın, ses seviyesini ayarlayın, müziği kontrol edin. Albüm kapağı, hava durumu, saat, sayaç ve CPU, GPU, RAM göstergelerini görüntüleyin. Kullanılabilir widget’lar Macro Deck kurulumunuza bağlıdır.
- **Temalar ve boşta ekran ayarı.** Macro Deck, Açık ve OLED Tam Siyah temalarından birini seçin. Üst durum çubuğunu ayarlayın. Konsol boştayken ekranı karartabilir, siyaha alabilir veya konsolu uyutabilirsiniz.
- **Yerel ağdan eşleştirme.** Macro Deck’i ağda bulun, altı haneli kodla eşleştirin ve kayıtlı oturumla yeniden bağlanın.

### Gereksinimler

- Özel yazılım (CFW) çalıştıran ve Homebrew Menu’ye erişebilen bir Nintendo Switch.
- Üzerinde [Macro Deck 3](https://macro-deck.app/) çalışan, en az bir widget veya eylem içeren profili olan bir bilgisayar.
- Switch ve bilgisayarın aynı yerel ağa bağlı olması.

### Kurulum

1. [GitHub Sürümler](https://github.com/sertay1/switchdeck/releases) sayfasından **SD ZIP** paketini indirin. İçindeki `switch` klasörünü SD kartın köküne kopyalayın. Depodaki kopyayı kullanıyorsanız aynı dizin yapısı `SD_Karta_Atilacak` altında bulunur.
2. Homebrew Menu’den **SwitchDeck**’i açın. Uygulama dosyasının yolu `sdmc:/switch/SwitchDeck/SwitchDeck.nro` olmalı.
3. **Ayarlar → Bağlantı & Sunucu** bölümünü açın. **Ağda Ara** düğmesini kullanın veya bilgisayarın yerel IP adresini ve sunucu portunu elle girin. Macro Deck 3’ün varsayılan portu `8193`.
4. Bilgisayarda Macro Deck’in **Cihazlar → Cihaz Ekle** bölümünden altı haneli eşleştirme kodu alın. Kodu Switch’e girip **Şimdi Bağlan** düğmesine basın.

SwitchDeck eşleştirmeyi SD kartta saklar. Sunucu adresini veya portu değiştirirseniz yeniden eşleştirin. Macro Deck 2 kullanıyorsanız kendi sunucu portunuzu girin ve eşleştirme kodunu boş bırakın.

### Kontroller

| Giriş | İşlev |
| --- | --- |
| Dokunmatik ekran | Widget’ı kullanır. Dokunma, çift dokunma ve basılı tutma işlevleri widget’a bağlıdır. |
| Yön tuşları / sol analog | Odağı taşır. |
| A | Odaktaki widget’ı tetikler. |
| B | Üst klasöre döner veya Ayarlar’ı kapatır. |
| L / R | Ana ekranda sayfayı, Ayarlar’da sekmeyi değiştirir. |
| ZL / ZR | Odaktaki kaydırıcının değerini azaltır veya artırır. |
| Y | Deck’i yeniler. Bağlantı kopmuşsa yeniden bağlanmayı dener. |
| X / − | Ayarlar’ı açar. Ayarlar açıkken − tuşu menüyü kapatır. |
| + | SwitchDeck’ten çıkar. |

### Kaynak koddan derleme

devkitPro, devkitA64, libnx ve [Makefile](Makefile) içindeki Switch portlibs bağımlılıklarını kurun. devkitPro kabuğunda `make` komutunu çalıştırarak `SwitchDeck.nro` dosyasını oluşturun. GitHub isteklerinde TLS doğrulaması için kullanılan [CA sertifika paketi](romfs/tls-ca-bundle.pem) NRO’ya gömülür. Ayrıntılar için [regresyon testi talimatlarına](tests/README.txt) bakın.

[Yukarı dön](#switchdeck) · [English](#english)

## Screenshots / Ekran görüntüleri

<table>
  <tr>
    <th>Connection &amp; Server / Bağlantı &amp; Sunucu</th>
    <th>Appearance &amp; Theme / Görünüm &amp; Tema</th>
  </tr>
  <tr>
    <td><a href="docs/images/connection-and-server.jpg"><img src="docs/images/connection-and-server.jpg" alt="SwitchDeck connection and pairing settings" width="440"></a></td>
    <td><a href="docs/images/appearance-and-theme.jpg"><img src="docs/images/appearance-and-theme.jpg" alt="SwitchDeck language, theme, and status bar settings" width="440"></a></td>
  </tr>
</table>

### About / Hakkında

![SwitchDeck About screen with the developer’s social links](docs/images/about.jpg)

**SwitchDeck by SertAy**
