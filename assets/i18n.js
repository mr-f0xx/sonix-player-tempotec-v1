(() => {
  const LANGUAGES = [
    { code: 'en', flag: '🇬🇧', label: 'EN', name: 'English' },
    { code: 'it', flag: '🇮🇹', label: 'IT', name: 'Italiano' },
    { code: 'de', flag: '🇩🇪', label: 'DE', name: 'Deutsch' },
    { code: 'es', flag: '🇪🇸', label: 'ES', name: 'Español' },
    { code: 'fr', flag: '🇫🇷', label: 'FR', name: 'Français' },
    { code: 'nl', flag: '🇳🇱', label: 'NL', name: 'Nederlands' },
    { code: 'pl', flag: '🇵🇱', label: 'PL', name: 'Polski' },
    { code: 'pt-BR', flag: '🇧🇷', label: 'PT', name: 'Português (BR)' },
    { code: 'uk', flag: '🇺🇦', label: 'UK', name: 'Українська' },
    { code: 'ru', flag: '🇷🇺', label: 'RU', name: 'Русский' },
    { code: 'ja', flag: '🇯🇵', label: 'JA', name: '日本語' },
  ];

  // Only interface copy lives here. Product names, feature names and GitHub
  // release notes stay in their original wording.
  const STRINGS = {
    it: {
      nav_features: 'FUNZIONALITÀ', nav_fork: 'AGGIUNTE DEL FORK', nav_upgrade: "L'AGGIORNAMENTO", nav_install: 'INSTALLAZIONE', nav_donate: 'DONAZIONI', nav_contact: 'CONTATTI / SOCIAL',
      hero_eyebrow: 'FIRMWARE COMUNITARIO', hero_title: 'IL TUO PLAYER.<br><em>Amplificato.</em>',
      hero_desc: '<strong>La tua musica, pronta quando vuoi.</strong> Sonix porta il tuo TempoTec V1 oltre lo stock 1.3: modella il suono per le tue cuffie, tieni la libreria sulla scheda e riscopri cosa può fare questo piccolo player.',
      btn_emulator: 'AVVIA EMULATORE WEB', btn_themes: 'SCARICA TEMI E ICONE', latest: 'ULTIMO AGGIORNAMENTO', release_date: 'DATA RILASCIO', release_notes: 'NOTE DI RILASCIO', view_release: 'VEDI RILASCIO <span>↗</span>',
      s1_kicker: "01 / L'ESPERIENZA", s1_kicker_right: 'PIÙ DI UN SEMPLICE PLAYER', s1_overline: 'IL V1, SBLOCCATO', s1_title: 'Piccolo dispositivo.<br><em>Grandi possibilità.</em>',
      s1_desc: "L'essenziale di Sonix, più aggiunte pensate per il V1. Creato dalla community, per il tuo modo di ascoltare.",
      s2_kicker: '02 / CREATO IN QUESTO FORK', s2_kicker_right: 'OLTRE IL SONIX ORIGINALE', s2_overline: 'FATTO PER IL TUO V1', s2_title: 'Non solo un port.<br><em>Molto di più.</em>',
      s2_desc: 'Questo fork si basa su Sonix upstream. Ecco alcune aggiunte e miglioramenti creati per il V1, non le funzioni già condivise con Sonix.',
      s3_kicker: "03 / L'AGGIORNAMENTO", s3_kicker_right: 'STOCK 1.3 CONTRO SONIX FORK', s3_overline: 'IL V1, AGGIORNATO', s3_title: 'Stesso hardware.<br><em>Nuovi orizzonti.</em>',
      s3_desc: 'Sonix parte dal firmware V1 1.3 ufficiale e sostituisce player e interfaccia. Lo stesso dispositivo, con molto più spazio per giocare.',
      matrix_header: 'MATRICE FUNZIONALITÀ', col_capability: 'FUNZIONALITÀ', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / PER INIZIARE', s4_kicker_right: 'LEGGI PRIMA DI AGGIORNARE', s4_overline: 'PRONTO QUANDO VUOI', s4_title: 'Fai il <em>passo.</em>',
      s4_desc: "Un po' di preparazione fa la differenza. Segui i passaggi qui sotto e conserva al sicuro i file originali.",
      safety_title: 'NOTA SULLA SICUREZZA', safety_desc: 'Solo per TempoTec Variations V1. Firmware comunitario non ufficiale: leggi istruzioni e avvertimenti prima di aggiornare.', read_guide: 'LEGGI LA GUIDA',
      s5_kicker: '05 / SUPPORTO', s5_kicker_right: 'MANTIENI VIVO IL FORK', s5_overline: 'TI PIACE SONIX?', s5_title: 'Sostieni il <em>lavoro.</em>',
      s5_desc: 'Questo fork è un progetto comunitario. Se il tuo V1 è diventato più divertente, puoi sostenere direttamente lo sviluppo.',
      footer_tagline: 'UN PROGETTO COMUNITARIO DI IMRE FERENCZI. CREATO PER AMORE DELLA MUSICA.',
      s6_kicker: '06 / CONTATTI', s6_kicker_right: 'TROVAMI ONLINE', c_github: 'Profilo GitHub', c_reddit: 'Profilo Reddit', c_soundcloud: 'Musica su SoundCloud',
    },
    de: {
      nav_features: 'FUNKTIONEN', nav_fork: 'FORK-ERGÄNZUNGEN', nav_upgrade: 'DAS UPGRADE', nav_install: 'INSTALLATION', nav_donate: 'SPENDEN', nav_contact: 'KONTAKT / SOCIAL MEDIA',
      hero_eyebrow: 'COMMUNITY-FIRMWARE', hero_title: 'DEIN PLAYER.<br><em>Verstärkt.</em>',
      hero_desc: '<strong>Deine Musik, bereit wenn du es bist.</strong> Sonix bringt deinen TempoTec V1 über Stock 1.3 hinaus: Forme den Klang für deine Kopfhörer, behalte deine Bibliothek auf der Karte und entdecke diesen kleinen Player neu.',
      btn_emulator: 'WEB-EMULATOR STARTEN', btn_themes: 'THEMEN & ICONS LADEN', latest: 'NEUESTES UPDATE', release_date: 'VERÖFFENTLICHT', release_notes: 'RELEASE-NOTIZEN', view_release: 'RELEASE ANSEHEN <span>↗</span>',
      s1_kicker: '01 / DAS ERLEBNIS', s1_kicker_right: 'MEHR ALS EIN MUSICPLAYER', s1_overline: 'DER V1, ENTSPERRT', s1_title: 'Kleines Gerät.<br><em>Große Möglichkeiten.</em>',
      s1_desc: 'Das Wesentliche von Sonix, plus Ergänzungen für den V1. Von der Community, für deine Art zu hören.',
      s2_kicker: '02 / IN DIESEM FORK ENTWICKELT', s2_kicker_right: 'JENSEITS DES ORIGINALEN SONIX', s2_overline: 'FÜR DEINEN V1 GEMACHT', s2_title: 'Nicht nur ein Port.<br><em>Ein ganzes Mehr.</em>',
      s2_desc: 'Dieser Fork baut auf upstream Sonix auf. Hier sind Ergänzungen und Verbesserungen für den V1 – keine Funktionen, die Sonix bereits hatte.',
      s3_kicker: '03 / DAS UPGRADE', s3_kicker_right: 'STOCK 1.3 GEGEN SONIX FORK', s3_overline: 'DER V1, AUFGERÜSTET', s3_title: 'Gleiche Hardware.<br><em>Neue Horizonte.</em>',
      s3_desc: 'Sonix beginnt mit der offiziellen V1-1.3-Firmware und ersetzt Player und Oberfläche. Das vertraute Gerät, mit viel mehr Spielraum.',
      matrix_header: 'FUNKTIONS-MATRIX', col_capability: 'FUNKTION', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / LOSLEGEN', s4_kicker_right: 'VOR DEM FLASHEN LESEN', s4_overline: 'BEREIT WENN DU ES BIST', s4_title: 'Mach den <em>Wechsel.</em>',
      s4_desc: 'Ein wenig Vorbereitung hilft viel. Folge den Schritten und bewahre deine Originaldateien sicher auf.',
      safety_title: 'SICHERHEITSHINWEIS', safety_desc: 'Nur für den TempoTec Variations V1. Inoffizielle Community-Firmware; lies die vollständigen Anweisungen und Warnungen vor dem Update.', read_guide: 'ANLEITUNG LESEN',
      s5_kicker: '05 / UNTERSTÜTZEN', s5_kicker_right: 'DEN FORK AM LEBEN HALTEN', s5_overline: 'GEFÄLLT DIR SONIX?', s5_title: 'Unterstütze die <em>Arbeit.</em>',
      s5_desc: 'Dieser Fork ist ein Community-Projekt. Wenn dein V1 mehr Spaß macht, kannst du die Entwicklung direkt unterstützen.',
      footer_tagline: 'EIN COMMUNITY-PROJEKT VON IMRE FERENCZI. AUS LIEBE ZUR MUSIK GEMACHT.',
      s6_kicker: '06 / KONTAKT', s6_kicker_right: 'FIND MICH ONLINE', c_github: 'GitHub-Profil', c_reddit: 'Reddit-Profil', c_soundcloud: 'Musik auf SoundCloud',
    },
    es: {
      nav_features: 'FUNCIONES', nav_fork: 'AÑADIDOS DEL FORK', nav_upgrade: 'LA ACTUALIZACIÓN', nav_install: 'INSTALAR', nav_donate: 'DONAR', nav_contact: 'CONTACTO / REDES SOCIALES',
      hero_eyebrow: 'FIRMWARE COMUNITARIO', hero_title: 'TU REPRODUCTOR.<br><em>Amplificado.</em>',
      hero_desc: '<strong>Tu música, lista cuando quieras.</strong> Sonix lleva tu TempoTec V1 más allá del stock 1.3: modela el sonido para tus auriculares, mantén tu biblioteca en la tarjeta y redescubre lo que puede hacer este pequeño reproductor.',
      btn_emulator: 'ABRIR EMULADOR WEB', btn_themes: 'DESCARGAR TEMAS E ICONOS', latest: 'ÚLTIMA ACTUALIZACIÓN', release_date: 'FECHA DE LANZAMIENTO', release_notes: 'NOTAS DE LA VERSIÓN', view_release: 'VER VERSIÓN <span>↗</span>',
      s1_kicker: '01 / LA EXPERIENCIA', s1_kicker_right: 'MÁS QUE UN REPRODUCTOR', s1_overline: 'EL V1, DESBLOQUEADO', s1_title: 'Pequeño dispositivo.<br><em>Grandes posibilidades.</em>',
      s1_desc: 'Lo esencial de Sonix, más añadidos pensados para el V1. Creado por la comunidad, para tu forma de escuchar.',
      s2_kicker: '02 / CREADO EN ESTE FORK', s2_kicker_right: 'MÁS ALLÁ DEL SONIX ORIGINAL', s2_overline: 'HECHO PARA TU V1', s2_title: 'No es solo un port.<br><em>Mucho más.</em>',
      s2_desc: 'Este fork se basa en Sonix original. Aquí tienes algunas adiciones y mejoras creadas para el V1, no funciones que Sonix ya compartía.',
      s3_kicker: '03 / LA ACTUALIZACIÓN', s3_kicker_right: 'STOCK 1.3 FRENTE A SONIX FORK', s3_overline: 'EL V1, MEJORADO', s3_title: 'Mismo hardware.<br><em>Nuevos horizontes.</em>',
      s3_desc: 'Sonix parte del firmware oficial V1 1.3 y sustituye el reproductor y la interfaz. El mismo dispositivo, con mucho más margen para jugar.',
      matrix_header: 'MATRIZ DE FUNCIONES', col_capability: 'FUNCIÓN', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / PARA EMPEZAR', s4_kicker_right: 'LEE ANTES DE FLASHAR', s4_overline: 'LISTO CUANDO QUIERAS', s4_title: 'Haz el <em>cambio.</em>',
      s4_desc: 'Un poco de preparación ayuda mucho. Sigue los pasos y guarda a salvo tus archivos originales.',
      safety_title: 'NOTA DE SEGURIDAD', safety_desc: 'Solo para TempoTec Variations V1. Firmware comunitario no oficial; lee las instrucciones y advertencias completas antes de actualizar.', read_guide: 'LEER LA GUÍA',
      s5_kicker: '05 / APOYO', s5_kicker_right: 'MANTÉN VIVO EL FORK', s5_overline: '¿TE GUSTA SONIX?', s5_title: 'Apoya el <em>trabajo.</em>',
      s5_desc: 'Este fork es un proyecto comunitario. Si tu V1 es más divertido, puedes apoyar directamente el desarrollo.',
      footer_tagline: 'UN PROYECTO COMUNITARIO DE IMRE FERENCZI. HECHO POR AMOR A LA MÚSICA.',
      s6_kicker: '06 / CONTACTO', s6_kicker_right: 'ENCUÉNTRAME EN LÍNEA', c_github: 'Perfil de GitHub', c_reddit: 'Perfil de Reddit', c_soundcloud: 'Música en SoundCloud',
    },
    fr: {
      nav_features: 'FONCTIONS', nav_fork: 'AJOUTS DU FORK', nav_upgrade: 'LA MISE À NIVEAU', nav_install: 'INSTALLATION', nav_donate: 'FAIRE UN DON', nav_contact: 'CONTACT / RÉSEAUX SOCIAUX',
      hero_eyebrow: 'FIRMWARE COMMUNAUTAIRE', hero_title: 'VOTRE LECTEUR.<br><em>Amplifié.</em>',
      hero_desc: '<strong>Votre musique, prête quand vous l’êtes.</strong> Sonix emmène votre TempoTec V1 au-delà de la version 1.3 : façonnez le son pour vos casques, gardez votre bibliothèque sur la carte et redécouvrez ce petit lecteur.',
      btn_emulator: 'LANCER L’ÉMULATEUR WEB', btn_themes: 'TÉLÉCHARGER THÈMES ET ICÔNES', latest: 'DERNIÈRE VERSION', release_date: 'DATE DE SORTIE', release_notes: 'NOTES DE VERSION', view_release: 'VOIR LA VERSION <span>↗</span>',
      s1_kicker: '01 / L’EXPÉRIENCE', s1_kicker_right: 'PLUS QU’UN LECTEUR', s1_overline: 'LE V1, DÉBLOQUÉ', s1_title: 'Petit appareil.<br><em>Grandes possibilités.</em>',
      s1_desc: 'L’essentiel de Sonix, plus des ajouts pensés pour le V1. Par la communauté, pour votre façon d’écouter.',
      s2_kicker: '02 / DANS CE FORK', s2_kicker_right: 'AU-DELÀ DU SONIX ORIGINAL', s2_overline: 'FAIT POUR VOTRE V1', s2_title: 'Pas simplement un port.<br><em>Bien plus.</em>',
      s2_desc: 'Ce fork repose sur Sonix amont. Voici des ajouts et améliorations créés pour le V1, et non les fonctions déjà partagées avec Sonix.',
      s3_kicker: '03 / LA MISE À NIVEAU', s3_kicker_right: 'STOCK 1.3 CONTRE SONIX FORK', s3_overline: 'LE V1, AMÉLIORÉ', s3_title: 'Même matériel.<br><em>De nouveaux horizons.</em>',
      s3_desc: 'Sonix part du firmware officiel V1 1.3 et remplace le lecteur et l’interface. Le même appareil, avec bien plus de liberté.',
      matrix_header: 'TABLEAU DES FONCTIONS', col_capability: 'FONCTION', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / POUR COMMENCER', s4_kicker_right: 'À LIRE AVANT LE FLASHAGE', s4_overline: 'PRÊT QUAND VOUS L’ÊTES', s4_title: 'Faites le <em>pas.</em>',
      s4_desc: 'Un peu de préparation change tout. Suivez les étapes et gardez vos fichiers d’origine en sécurité.',
      safety_title: 'NOTE DE SÉCURITÉ', safety_desc: 'Uniquement pour TempoTec Variations V1. Firmware communautaire non officiel : lisez les instructions et avertissements avant la mise à jour.', read_guide: 'LIRE LE GUIDE',
      s5_kicker: '05 / SOUTENIR', s5_kicker_right: 'FAITES VIVRE LE FORK', s5_overline: 'SONIX VOUS PLAÎT ?', s5_title: 'Soutenez le <em>travail.</em>',
      s5_desc: 'Ce fork est un projet communautaire. Si votre V1 est plus amusant, vous pouvez soutenir directement le développement.',
      footer_tagline: 'UN PROJET COMMUNAUTAIRE DE IMRE FERENCZI. PAR AMOUR DE LA MUSIQUE.',
      s6_kicker: '06 / CONTACT', s6_kicker_right: 'RETROUVEZ-MOI EN LIGNE', c_github: 'Profil GitHub', c_reddit: 'Profil Reddit', c_soundcloud: 'Musique sur SoundCloud',
    },
    nl: {
      nav_features: 'FUNCTIES', nav_fork: 'FORK-TOEVOEGINGEN', nav_upgrade: 'DE UPGRADE', nav_install: 'INSTALLEREN', nav_donate: 'DONEREN', nav_contact: 'CONTACT / SOCIALE MEDIA',
      hero_eyebrow: 'COMMUNITY-FIRMWARE', hero_title: 'JOUW SPELER.<br><em>Versterkt.</em>',
      hero_desc: '<strong>Je muziek, klaar wanneer jij klaar bent.</strong> Sonix brengt je TempoTec V1 voorbij stock 1.3: vorm het geluid voor jouw koptelefoons, houd je bibliotheek op de kaart en ontdek deze kleine speler opnieuw.',
      btn_emulator: 'WEB-EMULATOR STARTEN', btn_themes: 'THEMAS EN ICONEN DOWNLOADEN', latest: 'NIEUWSTE RELEASE', release_date: 'RELEASEDATUM', release_notes: 'RELEASENOTITIES', view_release: 'RELEASE BEKIJKEN <span>↗</span>',
      s1_kicker: '01 / DE ERVARING', s1_kicker_right: 'MEER DAN EEN MUZIEKSPELER', s1_overline: 'DE V1, ONTSLOTEN', s1_title: 'Klein apparaat.<br><em>Grote mogelijkheden.</em>',
      s1_desc: 'De essentie van Sonix, plus toevoegingen voor de V1. Door de community, voor jouw manier van luisteren.',
      s2_kicker: '02 / IN DEZE FORK GEMAAKT', s2_kicker_right: 'VOORBIJ DE OORSPRONKELIJKE SONIX', s2_overline: 'VOOR JOUW V1 GEMAAKT', s2_title: 'Niet zomaar een port.<br><em>Veel meer.</em>',
      s2_desc: 'Deze fork bouwt op upstream Sonix. Hier zijn toevoegingen en verbeteringen voor de V1, geen functies die Sonix al had.',
      s3_kicker: '03 / DE UPGRADE', s3_kicker_right: 'STOCK 1.3 TEGEN SONIX FORK', s3_overline: 'DE V1, OPGEWAARDEERD', s3_title: 'Zelfde hardware.<br><em>Nieuwe horizonnen.</em>',
      s3_desc: 'Sonix begint met de officiële V1 1.3-firmware en vervangt de speler en interface. Hetzelfde apparaat, met veel meer ruimte.',
      matrix_header: 'FUNCTIEMATRIX', col_capability: 'FUNCTIE', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / AAN DE SLAG', s4_kicker_right: 'LEES VOOR HET FLASHEN', s4_overline: 'KLAAR WANNEER JIJ KLAAR BENT', s4_title: 'Maak de <em>stap.</em>',
      s4_desc: 'Een kleine voorbereiding helpt veel. Volg de stappen en bewaar je originele bestanden veilig.',
      safety_title: 'VEILIGHEIDSNOTITIE', safety_desc: 'Alleen voor TempoTec Variations V1. Officiële community-firmware; lees de volledige instructies en waarschuwingen voor het updaten.', read_guide: 'LEES DE HANDLEIDING',
      s5_kicker: '05 / STEUN', s5_kicker_right: 'HOUD DE FORK LEVEND', s5_overline: 'VIND JE SONIX LEUK?', s5_title: 'Steun het <em>werk.</em>',
      s5_desc: 'Deze fork is een communityproject. Als je V1 leuker is geworden, kun je de ontwikkeling direct steunen.',
      footer_tagline: 'EEN COMMUNITYPROJECT DOOR IMRE FERENCZI. GEMAAKT UIT LIEFDE VOOR MUZIEK.',
      s6_kicker: '06 / CONTACT', s6_kicker_right: 'VIND ME ONLINE', c_github: 'GitHub-profiel', c_reddit: 'Reddit-profiel', c_soundcloud: 'Muziek op SoundCloud',
    },
    pl: {
      nav_features: 'FUNKCJE', nav_fork: 'DODATKI FORKA', nav_upgrade: 'AKTUALIZACJA', nav_install: 'INSTALACJA', nav_donate: 'WSPARCIE', nav_contact: 'KONTAKT / MEDIA SPOŁECZNOŚCIOWE',
      hero_eyebrow: 'FIRMWARE SPOŁECZNOŚCI', hero_title: 'TWÓJ ODTWARZACZ.<br><em>Wzmocniony.</em>',
      hero_desc: '<strong>Twoja muzyka, gotowa gdy ty jesteś.</strong> Sonix wyprowadza Twój TempoTec V1 poza stock 1.3: kształtuj dźwięk pod swoje słuchawki, trzymaj bibliotekę na karcie i odkryj ten mały odtwarzacz na nowo.',
      btn_emulator: 'URUCHOM EMULATOR WWW', btn_themes: 'POBIERZ MOTYWY I IKONY', latest: 'NAJNOWSZA WERSJA', release_date: 'DATA WYDANIA', release_notes: 'INFORMACJE O WERSJI', view_release: 'ZOBACZ WERSJĘ <span>↗</span>',
      s1_kicker: '01 / DOŚWIADCZENIE', s1_kicker_right: 'WIĘCEJ NIŻ ODTWARZACZ', s1_overline: 'V1, ODBLOKOWANY', s1_title: 'Małe urządzenie.<br><em>Wielkie możliwości.</em>',
      s1_desc: 'To, co najważniejsze w Sonix, plus dodatki dla V1. Stworzone przez społeczność, dla Twojego sposobu słuchania.',
      s2_kicker: '02 / POWSTAŁO W FORKU', s2_kicker_right: 'POZA ORYGINALNYM SONIXEM', s2_overline: 'ZROBIONE DLA TWOJEGO V1', s2_title: 'Nie tylko port.<br><em>O wiele więcej.</em>',
      s2_desc: 'Ten fork opiera się na Sonix upstream. To dodatki i ulepszenia stworzone dla V1, a nie funkcje które Sonix już miał.',
      s3_kicker: '03 / AKTUALIZACJA', s3_kicker_right: 'STOCK 1.3 VS SONIX FORK', s3_overline: 'V1, ULEPSZONY', s3_title: 'Ten sam sprzęt.<br><em>Nowe horyzonty.</em>',
      s3_desc: 'Sonix zaczyna od oficjalnego firmware V1 1.3 i zastępuje odtwarzacz oraz interfejs. To samo urządzenie, z dużo większymi możliwościami.',
      matrix_header: 'MACIERZ FUNKCJI', col_capability: 'FUNKCJA', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / ZACZNIJ', s4_kicker_right: 'PRZECZYTAJ PRZED FLASHEM', s4_overline: 'GOTOWY, KIEDY JESTEŚ', s4_title: 'Zrób <em>krok.</em>',
      s4_desc: 'Trochę przygotowania wiele zmienia. Wykonaj poniższe kroki i zachowaj oryginalne pliki.',
      safety_title: 'UWAGA DOTYCZĄCA BEZPIECZEŃSTWA', safety_desc: 'Tylko dla TempoTec Variations V1. To nieoficjalny firmware społeczności; przed aktualizacją przeczytaj pełne instrukcje i ostrzeżenia.', read_guide: 'PRZECZYTAJ PRZEWODNIK',
      s5_kicker: '05 / WSPARCIE', s5_kicker_right: 'UTRZYMAJ FORKA PRZY ŻYCIU', s5_overline: 'PODoba CI SIĘ SONIX?', s5_title: 'Wsparcie dla <em>pracy.</em>',
      s5_desc: 'Ten fork to projekt społeczności. Jeśli Twój V1 daje więcej frajdy, możesz bezpośrednio wspierać rozwój.',
      footer_tagline: 'PROJEKT SPOŁECZNOŚCI AUTORSTWA IMRE FERENCZIEGO. TWORZONY Z MIŁOŚCI DO MUZYKI.',
      s6_kicker: '06 / KONTAKT', s6_kicker_right: 'ZNAJDŹ MNIE ONLINE', c_github: 'Profil GitHub', c_reddit: 'Profil Reddit', c_soundcloud: 'Muzyka na SoundCloud',
    },
    'pt-BR': {
      nav_features: 'RECURSOS', nav_fork: 'ADIÇÕES DO FORK', nav_upgrade: 'A ATUALIZAÇÃO', nav_install: 'INSTALAR', nav_donate: 'DOAR', nav_contact: 'CONTATO / REDES SOCIAIS',
      hero_eyebrow: 'FIRMWARE DA COMUNIDADE', hero_title: 'SEU PLAYER.<br><em>Amplificado.</em>',
      hero_desc: '<strong>Sua música, pronta quando você quiser.</strong> O Sonix leva seu TempoTec V1 além do stock 1.3: molde o som para seus fones, mantenha sua biblioteca no cartão e redescubra o que este pequeno player pode fazer.',
      btn_emulator: 'ABRIR EMULADOR WEB', btn_themes: 'BAIXAR TEMAS E ÍCONES', latest: 'ÚLTIMO LANÇAMENTO', release_date: 'DATA DO LANÇAMENTO', release_notes: 'NOTAS DA VERSÃO', view_release: 'VER LANÇAMENTO <span>↗</span>',
      s1_kicker: '01 / A EXPERIÊNCIA', s1_kicker_right: 'MAIS QUE UM PLAYER', s1_overline: 'O V1, DESBLOQUEADO', s1_title: 'Pequeno aparelho.<br><em>Grandes possibilidades.</em>',
      s1_desc: 'O essencial do Sonix, mais adições feitas para o V1. Pela comunidade, para o seu jeito de ouvir.',
      s2_kicker: '02 / CRIADO NESTE FORK', s2_kicker_right: 'ALÉM DO SONIX ORIGINAL', s2_overline: 'FEITO PARA O SEU V1', s2_title: 'Não é só um port.<br><em>Muito mais.</em>',
      s2_desc: 'Este fork parte do Sonix upstream. Aqui estão adições e melhorias criadas para o V1, e não recursos que o Sonix já tinha.',
      s3_kicker: '03 / A ATUALIZAÇÃO', s3_kicker_right: 'STOCK 1.3 VS SONIX FORK', s3_overline: 'O V1, MELHORADO', s3_title: 'Mesmo hardware.<br><em>Novos horizontes.</em>',
      s3_desc: 'O Sonix começa com o firmware oficial V1 1.3 e substitui o player e a interface. O mesmo aparelho, com muito mais espaço para brincar.',
      matrix_header: 'MATRIZ DE RECURSOS', col_capability: 'RECURSO', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / COMEÇAR', s4_kicker_right: 'LEIA ANTES DE INSTALAR', s4_overline: 'PRONTO QUANDO VOCÊ QUISER', s4_title: 'Faça a <em>mudança.</em>',
      s4_desc: 'Um pouco de preparação ajuda muito. Siga os passos abaixo e mantenha seus arquivos originais seguros.',
      safety_title: 'NOTA DE SEGURANÇA', safety_desc: 'Apenas para TempoTec Variations V1. Este é um firmware comunitário não oficial; leia as instruções e avisos completos antes de atualizar.', read_guide: 'LER O GUIA',
      s5_kicker: '05 / APOIO', s5_kicker_right: 'MANTENHA O FORK VIVO', s5_overline: 'GOSTA DO SONIX?', s5_title: 'Apoie o <em>trabalho.</em>',
      s5_desc: 'Este fork é um projeto comunitário. Se o seu V1 ficou mais divertido, você pode apoiar o desenvolvimento diretamente.',
      footer_tagline: 'UM PROJETO COMUNITÁRIO POR IMRE FERENCZI. FEITO POR AMOR À MÚSICA.',
      s6_kicker: '06 / CONTATO', s6_kicker_right: 'ME ENCONTRE ONLINE', c_github: 'Perfil no GitHub', c_reddit: 'Perfil no Reddit', c_soundcloud: 'Música no SoundCloud',
    },
    uk: {
      nav_features: 'ФУНКЦІЇ', nav_fork: 'ДОДАТКИ ФОРКА', nav_upgrade: 'ОНОВЛЕННЯ', nav_install: 'ВСТАНОВЛЕННЯ', nav_donate: 'ПІДТРИМКА', nav_contact: 'КОНТАКТИ / СОЦМЕРЕЖІ',
      hero_eyebrow: 'ГРОМАДСЬКА ПРОШИВКА', hero_title: 'ВАШ ПЛЕЄР.<br><em>Підсилений.</em>',
      hero_desc: '<strong>Ваша музика, готова, коли ви готові.</strong> Sonix виводить ваш TempoTec V1 за межі stock 1.3: налаштовуйте звук під свої навушники, тримайте бібліотеку на карті та відкрийте цей маленький плеєр заново.',
      btn_emulator: 'ЗАПУСТИТИ ВЕБ-ЕМУЛЯТОР', btn_themes: 'ЗАВАНТАЖИТИ ТЕМИ ТА ІКОНКИ', latest: 'ОСТАННЯ ВЕРСІЯ', release_date: 'ДАТА ВИПУСКУ', release_notes: 'ПРИМІТКИ ДО ВЕРСІЇ', view_release: 'ДИВИТИСЯ ВЕРСІЮ <span>↗</span>',
      s1_kicker: '01 / ДОСВІД', s1_kicker_right: 'БІЛЬШЕ, НІЖ ПЛЕЄР', s1_overline: 'V1, РОЗБЛОКОВАНИЙ', s1_title: 'Малий пристрій.<br><em>Великі можливості.</em>',
      s1_desc: 'Основи Sonix плюс додатки для V1. Створено громадою для вашого способу слухати.',
      s2_kicker: '02 / СТВОРЕНО У ФОРКУ', s2_kicker_right: 'ПОЗА ОРИГІНАЛЬНИМ SONIX', s2_overline: 'ЗРОБЛЕНО ДЛЯ ВАШОГО V1', s2_title: 'Не просто порт.<br><em>Набагато більше.</em>',
      s2_desc: 'Цей форк побудований на Sonix upstream. Тут додатки та покращення для V1, а не функції, які вже були в Sonix.',
      s3_kicker: '03 / ОНОВЛЕННЯ', s3_kicker_right: 'STOCK 1.3 ПРОТИ SONIX FORK', s3_overline: 'V1, ОНОВЛЕНИЙ', s3_title: 'Те саме залізо.<br><em>Нові горизонти.</em>',
      s3_desc: 'Sonix починається з офіційної прошивки V1 1.3 і замінює плеєр та інтерфейс. Знайомий пристрій із значно більшими можливостями.',
      matrix_header: 'МАТРИЦЯ ФУНКЦІЙ', col_capability: 'ФУНКЦІЯ', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / ПОЧНИМО', s4_kicker_right: 'ПРОЧИТАЙТЕ ПЕРЕД ПРОШИВКОЮ', s4_overline: 'ГОТОВИЙ, КОЛИ ВИ ГОТОВІ', s4_title: 'Зробіть <em>крок.</em>',
      s4_desc: 'Невелика підготовка багато змінює. Виконайте кроки нижче й збережіть оригінальні файли.',
      safety_title: 'ПРИМІТКА ЩОДО БЕЗПЕКИ', safety_desc: 'Тільки для TempoTec Variations V1. Це неофіційна громадська прошивка; перед оновленням прочитайте повні інструкції та попередження.', read_guide: 'ЧИТАТИ ІНСТРУКЦІЮ',
      s5_kicker: '05 / ПІДТРИМКА', s5_kicker_right: 'ПІДТРИМУЙТЕ FORK', s5_overline: 'ПОДОБАЄТЬСЯ SONIX?', s5_title: 'Підтримайте <em>роботу.</em>',
      s5_desc: 'Цей форк — громадський проект. Якщо ваш V1 став цікавішим, ви можете напряму підтримати розвиток.',
      footer_tagline: 'ГРОМАДСЬКИЙ ПРОЕКТ ВІД IMRE FERENCZI. СТВОРЕНИЙ З ЛЮБОВІ ДО МУЗИКИ.',
      s6_kicker: '06 / КОНТАКТИ', s6_kicker_right: 'ЗНАЙДИ МЕНЕ ОНЛАЙН', c_github: 'Профіль GitHub', c_reddit: 'Профіль Reddit', c_soundcloud: 'Музика на SoundCloud',
    },
    ru: {
      nav_features: 'ВОЗМОЖНОСТИ', nav_fork: 'ДОПОЛНЕНИЯ ФОРКА', nav_upgrade: 'ОБНОВЛЕНИЕ', nav_install: 'УСТАНОВКА', nav_donate: 'ПОДДЕРЖКА', nav_contact: 'КОНТАКТЫ / СОЦСЕТИ',
      hero_eyebrow: 'СООБЩЕСТВЕННАЯ ПРОШИВКА', hero_title: 'ВАШ ПЛЕЕР.<br><em>Усиленный.</em>',
      hero_desc: '<strong>Ваша музыка, готова, когда вы готовы.</strong> Sonix выводит ваш TempoTec V1 за пределы stock 1.3: настраивайте звук под свои наушники, держите библиотеку на карте и заново откройте этот маленький плеер.',
      btn_emulator: 'ЗАПУСТИТЬ ВЕБ-ЭМУЛЯТОР', btn_themes: 'СКАЧАТЬ ТЕМЫ И ИКОНКИ', latest: 'ПОСЛЕДНЯЯ ВЕРСИЯ', release_date: 'ДАТА ВЫПУСКА', release_notes: 'СПИСОК ИЗМЕНЕНИЙ', view_release: 'СМОТРЕТЬ ВЕРСИЮ <span>↗</span>',
      s1_kicker: '01 / ОПЫТ', s1_kicker_right: 'БОЛЬШЕ, ЧЕМ ПЛЕЕР', s1_overline: 'V1, РАЗБЛОКИРОВАННЫЙ', s1_title: 'Маленькое устройство.<br><em>Большие возможности.</em>',
      s1_desc: 'Основы Sonix плюс дополнения для V1. Создано сообществом для вашего способа слушать.',
      s2_kicker: '02 / СОЗДАНО В ФОРКЕ', s2_kicker_right: 'ПОЗАДИ ОРИГИНАЛЬНОГО SONIX', s2_overline: 'СДЕЛАНО ДЛЯ ВАШЕГО V1', s2_title: 'Не просто порт.<br><em>Гораздо больше.</em>',
      s2_desc: 'Этот форк построен на Sonix upstream. Здесь дополнения и улучшения для V1, а не функции, которые уже были в Sonix.',
      s3_kicker: '03 / ОБНОВЛЕНИЕ', s3_kicker_right: 'STOCK 1.3 ПРОТИВ SONIX FORK', s3_overline: 'V1, УЛУЧШЕННЫЙ', s3_title: 'То же железо.<br><em>Новые горизонты.</em>',
      s3_desc: 'Sonix начинается с официальной прошивки V1 1.3 и заменяет плеер и интерфейс. Знакомое устройство с гораздо большими возможностями.',
      matrix_header: 'МАТРИЦА ФУНКЦИЙ', col_capability: 'ФУНКЦИЯ', col_stock: 'STOCK 1.3', col_fork: 'SONIX FORK',
      s4_kicker: '04 / НАЧНЁМ', s4_kicker_right: 'ПРОЧТИТЕ ПЕРЕД ПРОШИВКОЙ', s4_overline: 'ГОТОВ, КОГДА ВЫ ГОТОВЫ', s4_title: 'Сделайте <em>шаг.</em>',
      s4_desc: 'Небольшая подготовка многое меняет. Выполните шаги ниже и сохраните оригинальные файлы.',
      safety_title: 'ПРИМЕЧАНИЕ О БЕЗОПАСНОСТИ', safety_desc: 'Только для TempoTec Variations V1. Это неофициальная прошивка сообщества; перед обновлением прочитайте полные инструкции и предупреждения.', read_guide: 'ЧИТАТЬ РУКОВОДСТВО',
      s5_kicker: '05 / ПОДДЕРЖКА', s5_kicker_right: 'ПОДДЕРЖИВАЙТЕ FORK', s5_overline: 'НРАВИТСЯ SONIX?', s5_title: 'Поддержите <em>работу.</em>',
      s5_desc: 'Этот форк — проект сообщества. Если ваш V1 стал интереснее, вы можете напрямую поддержать разработку.',
      footer_tagline: 'ПРОЕКТ СООБЩЕСТВА ОТ IMRE FERENCZI. СОЗДАН ПО ЛЮБВИ К МУЗЫКЕ.',
      s6_kicker: '06 / КОНТАКТЫ', s6_kicker_right: 'НАЙДИ МЕНЯ ОНЛАЙН', c_github: 'Профиль GitHub', c_reddit: 'Профиль Reddit', c_soundcloud: 'Музыка на SoundCloud',
    },
    ja: {
      nav_features: '機能', nav_fork: 'フォーク追加機能', nav_upgrade: 'アップグレード', nav_install: 'インストール', nav_donate: '寄付', nav_contact: 'お問い合わせ / SNS',
      hero_eyebrow: 'コミュニティファームウェア', hero_title: 'あなたのプレイヤー。<br><em>さらにパワフルに。</em>',
      hero_desc: '<strong>あなたの音楽を、いつでも。</strong> SonixはTempoTec V1を純正1.3の先へ連れて行きます。ヘッドホンに合わせた音作り、カード上のライブラリ、この小さなプレイヤーの再発見を。',
      btn_emulator: 'ウェブエミュレータを起動', btn_themes: 'テーマとアイコンをダウンロード', latest: '最新リリース', release_date: '公開日', release_notes: 'リリースノート', view_release: 'リリースを見る <span>↗</span>',
      s1_kicker: '01 / 体験', s1_kicker_right: 'ただの音楽プレイヤーではない', s1_overline: 'V1を解放する', s1_title: '小さな本体。<br><em>大きな可能性。</em>',
      s1_desc: 'Sonixの基本機能に、V1のための追加機能。コミュニティが作った、あなたの聴き方に合わせたプレイヤー。',
      s2_kicker: '02 / このフォークで追加', s2_kicker_right: 'オリジナルSonixの先へ', s2_overline: 'あなたのV1のために', s2_title: '単なる移植ではない。<br><em>それ以上のもの。</em>',
      s2_desc: 'このフォークは上流のSonixをベースにしています。Sonixと共通の機能ではなく、V1のために作られた追加と改善を紹介します。',
      s3_kicker: '03 / アップグレード', s3_kicker_right: '純正1.3 対 Sonixフォーク', s3_overline: 'V1をアップグレード', s3_title: '同じハードウェア。<br><em>新しい地平。</em>',
      s3_desc: 'Sonixは公式V1 1.3ファームウェアを起点に、プレイヤーとインターフェースを置き換えます。おなじみの端末に、もっと自由な遊び場を。',
      matrix_header: '機能マトリックス', col_capability: '機能', col_stock: '純正1.3', col_fork: 'Sonixフォーク',
      s4_kicker: '04 / はじめる', s4_kicker_right: '書き換え前に読んでください', s4_overline: 'いつでも準備OK', s4_title: '<em>切り替え</em>を。',
      s4_desc: '少しの準備が大きな違いを生みます。以下の手順に従い、純正ファイルを安全に保管してください。',
      safety_title: '安全上の注意', safety_desc: 'TempoTec Variations V1専用です。非公式のコミュニティファームウェアです。更新前に完全な説明と警告をお読みください。', read_guide: 'ガイドを読む',
      s5_kicker: '05 / サポート', s5_kicker_right: 'FORK を育て続ける', s5_overline: 'Sonixを気に入りましたか？', s5_title: 'この<em>活動</em>を応援。',
      s5_desc: 'このフォークはコミュニティプロジェクトです。V1がより楽しくなったなら、開発を直接支援できます。',
      footer_tagline: 'IMRE FERENCZI によるコミュニティプロジェクト。音楽への愛から生まれました。',
      s6_kicker: '06 / お問い合わせ', s6_kicker_right: 'オンラインで見つける', c_github: 'GitHub プロフィール', c_reddit: 'Reddit プロフィール', c_soundcloud: 'SoundCloud の音楽',
    },
  };

  const EXTRA = window.SONIX_I18N_EXTRA;
  if (EXTRA) {
    (EXTRA.keys || []).forEach((key, index) => {
      Object.keys(EXTRA.translations || {}).forEach(code => {
        STRINGS[code] = STRINGS[code] || {};
        if (!STRINGS[code][key]) STRINGS[code][key] = EXTRA.translations[code][index];
      });
    });
  }

  const DYNAMIC_EN = {
    btn_download: 'DOWNLOAD FIRMWARE',
    nav_menu: 'MENU',
    nav_close: 'CLOSE',
    rel_unavailable: 'Release unavailable',
    rel_failed: 'Could not load release details. Visit GitHub to download the firmware.',
    rel_no_asset: ' No .upt asset found; check the release page.',
    rel_notes_external: 'See the release on GitHub for full details.',
  };

  const root = document.documentElement;
  const wrapper = document.querySelector('#lang-switcher');
  const button = document.querySelector('#lang-button');
  const menu = document.querySelector('#lang-menu');
  const flag = document.querySelector('#lang-flag');
  const storageKey = 'sonix-site-language';
  let current = 'en';

  window.sonixI18n = {
    t(key) {
      const value = (STRINGS[current] || {})[key];
      return value || DYNAMIC_EN[key] || '';
    },
  };

  function rememberOriginals() {
    document.querySelectorAll('[data-i18n], [data-i18n-html]').forEach(element => {
      if (element.dataset.i18nOriginal === undefined) {
        element.dataset.i18nOriginal = element.hasAttribute('data-i18n-html') ? element.innerHTML : element.textContent;
      }
    });
  }

  function apply(lang) {
    rememberOriginals();
    const dictionary = STRINGS[lang] || {};
    document.querySelectorAll('[data-i18n]').forEach(element => {
      const value = dictionary[element.dataset.i18n];
      element.textContent = value || element.dataset.i18nOriginal;
    });
    document.querySelectorAll('[data-i18n-html]').forEach(element => {
      const value = dictionary[element.dataset.i18nHtml];
      element.innerHTML = value || element.dataset.i18nOriginal;
    });
    const selected = LANGUAGES.find(item => item.code === lang) || LANGUAGES[0];
    current = selected.code;
    root.lang = current;
    if (flag) flag.textContent = selected.flag;
    if (button) button.setAttribute('aria-label', 'Language: ' + selected.name);
    if (menu) {
      menu.querySelectorAll('[data-lang]').forEach(item => {
        const active = item.dataset.lang === current;
        item.classList.toggle('active', active);
        item.setAttribute('aria-checked', String(active));
      });
    }
    try { localStorage.setItem(storageKey, current); } catch (_) {}
    document.dispatchEvent(new CustomEvent('sonix:languagechange', { detail: { lang: current } }));
  }

  if (menu) {
    menu.innerHTML = LANGUAGES.map(item =>
      '<button type="button" role="menuitemradio" data-lang="' + item.code + '">' +
      '<span class="lang-menu-flag">' + item.flag + '</span><span>' + item.name + '</span></button>'
    ).join('');
    menu.addEventListener('click', event => {
      const choice = event.target.closest('[data-lang]');
      if (!choice) return;
      apply(choice.dataset.lang);
      closeMenu();
    });
  }

  function closeMenu() {
    if (!menu || !button) return;
    menu.classList.remove('open');
    button.setAttribute('aria-expanded', 'false');
  }

  if (button) {
    button.addEventListener('click', event => {
      event.stopPropagation();
      const open = !menu.classList.contains('open');
      menu.classList.toggle('open', open);
      button.setAttribute('aria-expanded', String(open));
    });
  }

  document.addEventListener('click', event => {
    if (wrapper && !wrapper.contains(event.target)) closeMenu();
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape') closeMenu();
  });

  let saved = 'en';
  try { saved = localStorage.getItem(storageKey) || 'en'; } catch (_) {}
  apply(LANGUAGES.some(item => item.code === saved) ? saved : 'en');
})();
