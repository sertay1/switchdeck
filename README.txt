Widget regresyon kontrolleri

Proje kökünden:
  python tests/generate.py
  g++ -std=c++17 -Itests/stubs -Iinclude -Isource tests/generated.cpp source/widget_localization.cpp source/config.cpp -o widget-tests
  ./widget-tests

Windows'ta son komut widget-tests.exe olarak çalıştırılır. Clang++ da kullanılabilir.

generate.py ilgili üretim fonksiyonlarını doğrudan kaynak dosyalardan alır.
Gerçek Macro Deck yanıtlarından hazırlanmış, oturum ve cihaz kimlikleri çıkarılmış
test verileri tests/fixtures altındadır. Testlere SDL veya Switch kurulumu gerekmez.

Kontroller: widget oturumu kurtarma, komut reddi ve geri bildirim, eski Spotify
kapağının temizlenmesi, kesilen ayar yazımından kurtarma; WebSocket ping yanıtı
eşleştirmesi ve gecikme süzme; çeviri ve
argümanlar; hava durumu; yüzde metni; geri sayımın boşta,
çalışan ve duraklatılmış halleri; kronometre; Türkçe tarih; saat modu değişince
eski oturumun bırakılması; boşta Spotify Play yönlendirmesi; yazı hizalaması;
dikey yüzde konumu; üç göstergenin boyutu/hizası; desteklenen 12 pil/cihaz ikonu.

Sınırlar: yazı boyutları ve çizim çağrıları taklit edilir. Bu testler gerçek SDL
piksel çıktısını, Switch donanımını, Spotify/Spicetify bağlantısını veya gerçek
şarkı başlatmayı doğrulamaz. Saat oturumu testinde ağ ve profil ayrıştırıcısı
yerine küçük test karşılıkları kullanılır; test edilen güncelleme fonksiyonu
üretim kaynağından alınır.
