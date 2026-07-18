#include "Parsing/Fb2GenreMapper.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace InpxWebReader::Fb2Parsing {
namespace {

constexpr std::string_view GGenre19thCenturyLiterature = "19th Century Literature";
constexpr std::string_view GGenre20thCenturyLiterature = "20th Century Literature";
constexpr std::string_view GGenreAbsurdistProse = "Absurdist Prose";
constexpr std::string_view GGenreAccounting = "Accounting";
constexpr std::string_view GGenreActionFantasy = "Action Fantasy";
constexpr std::string_view GGenreActionFiction = "Action Fiction";
constexpr std::string_view GGenreActionSF = "Action SF";
constexpr std::string_view GGenreAdultFiction = "Adult Fiction";
constexpr std::string_view GGenreAdventure = "Adventure";
constexpr std::string_view GGenreAdventureFantasy = "Adventure Fantasy";
constexpr std::string_view GGenreAdventureLiterature = "Adventure Literature";
constexpr std::string_view GGenreAdventureStories = "Adventure Stories";
constexpr std::string_view GGenreAgriculture = "Agriculture";
constexpr std::string_view GGenreAlternativeHistory = "Alternative History";
constexpr std::string_view GGenreAlternativeHistoryFantasy = "Alternative History Fantasy";
constexpr std::string_view GGenreAlternativeMedicine = "Alternative Medicine";
constexpr std::string_view GGenreAmericanLiterature = "American Literature";
constexpr std::string_view GGenreAncientHistory = "Ancient History";
constexpr std::string_view GGenreAnecdotes = "Anecdotes";
constexpr std::string_view GGenreAnimalAdventure = "Animal Adventure";
constexpr std::string_view GGenreAnimalDetectives = "Animal Detectives";
constexpr std::string_view GGenreAnimestyleFiction = "Anime-style Fiction";
constexpr std::string_view GGenreAnthology = "Anthology";
constexpr std::string_view GGenreAnthropology = "Anthropology";
constexpr std::string_view GGenreAntique = "Antique";
constexpr std::string_view GGenreAntiqueLiterature = "Antique Literature";
constexpr std::string_view GGenreAphorisms = "Aphorisms";
constexpr std::string_view GGenreArchaeology = "Archaeology";
constexpr std::string_view GGenreArchitecture = "Architecture";
constexpr std::string_view GGenreArt = "Art";
constexpr std::string_view GGenreArtDesign = "Art & Design";
constexpr std::string_view GGenreArtCriticism = "Art Criticism";
constexpr std::string_view GGenreAsianFantasy = "Asian Fantasy";
constexpr std::string_view GGenreAstrology = "Astrology";
constexpr std::string_view GGenreAstronomyCosmology = "Astronomy & Cosmology";
constexpr std::string_view GGenreAutomotiveRegulations = "Automotive Regulations";
constexpr std::string_view GGenreBiochemistry = "Biochemistry";
constexpr std::string_view GGenreBiography = "Biography";
constexpr std::string_view GGenreBiographyMemoirs = "Biography & Memoirs";
constexpr std::string_view GGenreBiology = "Biology";
constexpr std::string_view GGenreBiophysics = "Biophysics";
constexpr std::string_view GGenreBotany = "Botany";
constexpr std::string_view GGenreBritishLiterature = "British Literature";
constexpr std::string_view GGenreBuddhism = "Buddhism";
constexpr std::string_view GGenreBusiness = "Business";
constexpr std::string_view GGenreBusinessLifestyle = "Business & Lifestyle";
constexpr std::string_view GGenreBusinessManagement = "Business Management";
constexpr std::string_view GGenreCareerJobSearch = "Career & Job Search";
constexpr std::string_view GGenreCatholicism = "Catholicism";
constexpr std::string_view GGenreChemistry = "Chemistry";
constexpr std::string_view GGenreChildPsychology = "Child Psychology";
constexpr std::string_view GGenreChildrens = "Children's";
constexpr std::string_view GGenreChildrensAdventures = "Children's Adventures";
constexpr std::string_view GGenreChildrensCharacters = "Children's Characters";
constexpr std::string_view GGenreChildrensClassics = "Children's Classics";
constexpr std::string_view GGenreChildrensDetectives = "Children's Detectives";
constexpr std::string_view GGenreChildrensFantasy = "Children's Fantasy";
constexpr std::string_view GGenreChildrensFolklore = "Children's Folklore";
constexpr std::string_view GGenreChildrensHistoricalProse = "Children's Historical Prose";
constexpr std::string_view GGenreChildrensHistory = "Children's History";
constexpr std::string_view GGenreChildrensHumor = "Children's Humor";
constexpr std::string_view GGenreChildrensProse = "Children's Prose";
constexpr std::string_view GGenreChildrensScienceFiction = "Children's Science Fiction";
constexpr std::string_view GGenreChildrensVerses = "Children's Verses";
constexpr std::string_view GGenreChristianEducation = "Christian Education";
constexpr std::string_view GGenreChristianFiction = "Christian Fiction";
constexpr std::string_view GGenreChristianity = "Christianity";
constexpr std::string_view GGenreCinema = "Cinema";
constexpr std::string_view GGenreCinemaTheatre = "Cinema & Theatre";
constexpr std::string_view GGenreClassicLiterature = "Classic Literature";
constexpr std::string_view GGenreClassicProse = "Classic Prose";
constexpr std::string_view GGenreClassicalDetectives = "Classical Detectives";
constexpr std::string_view GGenreClassicalPoetry = "Classical Poetry";
constexpr std::string_view GGenreCollecting = "Collecting";
constexpr std::string_view GGenreCollection = "Collection";
constexpr std::string_view GGenreComedy = "Comedy";
constexpr std::string_view GGenreComicsGraphicNovels = "Comics & Graphic Novels";
constexpr std::string_view GGenreComputerEducation = "Computer Education";
constexpr std::string_view GGenreComputersInternet = "Computers & Internet";
constexpr std::string_view GGenreConservation = "Conservation";
constexpr std::string_view GGenreConstructionArchitecture = "Construction & Architecture";
constexpr std::string_view GGenreContemporaryFiction = "Contemporary Fiction";
constexpr std::string_view GGenreContemporaryForeignFiction = "Contemporary Foreign Fiction";
constexpr std::string_view GGenreContemporaryProse = "Contemporary Prose";
constexpr std::string_view GGenreContemporaryRomance = "Contemporary Romance";
constexpr std::string_view GGenreContemporaryRussianFiction = "Contemporary Russian Fiction";
constexpr std::string_view GGenreCooking = "Cooking";
constexpr std::string_view GGenreCounterculture = "Counterculture";
constexpr std::string_view GGenreCozyMysteries = "Cozy Mysteries";
constexpr std::string_view GGenreCrime = "Crime";
constexpr std::string_view GGenreCrimeNonFiction = "Crime Non-Fiction";
constexpr std::string_view GGenreCriminology = "Criminology";
constexpr std::string_view GGenreCriticism = "Criticism";
constexpr std::string_view GGenreCulturalStudies = "Cultural Studies";
constexpr std::string_view GGenreCyberpunk = "Cyberpunk";
constexpr std::string_view GGenreDance = "Dance";
constexpr std::string_view GGenreDarkFantasy = "Dark Fantasy";
constexpr std::string_view GGenreDatabases = "Databases";
constexpr std::string_view GGenreDetective = "Detective";
constexpr std::string_view GGenreDetectiveAction = "Detective Action";
constexpr std::string_view GGenreDetectiveRomance = "Detective Romance";
constexpr std::string_view GGenreDetectives = "Detectives";
constexpr std::string_view GGenreDictionaries = "Dictionaries";
constexpr std::string_view GGenreDissidentLiterature = "Dissident Literature";
constexpr std::string_view GGenreDoItYourself = "Do It Yourself";
constexpr std::string_view GGenreDragonFantasy = "Dragon Fantasy";
constexpr std::string_view GGenreDrama = "Drama";
constexpr std::string_view GGenreDramaturgy = "Dramaturgy";
constexpr std::string_view GGenreDystopian = "Dystopian";
constexpr std::string_view GGenreEarthSciences = "Earth Sciences";
constexpr std::string_view GGenreEasternPoetry = "Eastern Poetry";
constexpr std::string_view GGenreEasternReligions = "Eastern Religions";
constexpr std::string_view GGenreEcology = "Ecology";
constexpr std::string_view GGenreEconomics = "Economics";
constexpr std::string_view GGenreEducation = "Education";
constexpr std::string_view GGenreEducational = "Educational";
constexpr std::string_view GGenreEncyclopedias = "Encyclopedias";
constexpr std::string_view GGenreEntertaining = "Entertaining";
constexpr std::string_view GGenreEntertainmentHumor = "Entertainment & Humor";
constexpr std::string_view GGenreEpic = "Epic";
constexpr std::string_view GGenreEpicFantasy = "Epic Fantasy";
constexpr std::string_view GGenreEpicPoetry = "Epic Poetry";
constexpr std::string_view GGenreEpistolaryFiction = "Epistolary Fiction";
constexpr std::string_view GGenreEroticFantasy = "Erotic Fantasy";
constexpr std::string_view GGenreEroticRomance = "Erotic Romance";
constexpr std::string_view GGenreErotica = "Erotica";
constexpr std::string_view GGenreEroticaSex = "Erotica & Sex";
constexpr std::string_view GGenreEsoterics = "Esoterics";
constexpr std::string_view GGenreEspionage = "Espionage";
constexpr std::string_view GGenreEssays = "Essays";
constexpr std::string_view GGenreEuropeanAntique = "European Antique";
constexpr std::string_view GGenreEverydayFantasy = "Everyday Fantasy";
constexpr std::string_view GGenreExperimentalFiction = "Experimental Fiction";
constexpr std::string_view GGenreExperimentalPoetry = "Experimental Poetry";
constexpr std::string_view GGenreExperimentalProse = "Experimental Prose";
constexpr std::string_view GGenreExtravaganza = "Extravaganza";
constexpr std::string_view GGenreFables = "Fables";
constexpr std::string_view GGenreFairyTales = "Fairy Tales";
constexpr std::string_view GGenreFamily = "Family";
constexpr std::string_view GGenreFamilyHealth = "Family Health";
constexpr std::string_view GGenreFanTranslation = "Fan Translation";
constexpr std::string_view GGenreFanfiction = "Fanfiction";
constexpr std::string_view GGenreFantasy = "Fantasy";
constexpr std::string_view GGenreFantasyHumor = "Fantasy & Humor";
constexpr std::string_view GGenreFantasyDetective = "Fantasy Detective";
constexpr std::string_view GGenreFantasyRomance = "Fantasy Romance";
constexpr std::string_view GGenreFiction = "Fiction";
constexpr std::string_view GGenreFinanceBanking = "Finance & Banking";
constexpr std::string_view GGenreFinanceInvesting = "Finance & Investing";
constexpr std::string_view GGenreFolkSongs = "Folk Songs";
constexpr std::string_view GGenreFolkTales = "Folk Tales";
constexpr std::string_view GGenreFolklore = "Folklore";
constexpr std::string_view GGenreForeignActionFiction = "Foreign Action Fiction";
constexpr std::string_view GGenreForeignBusiness = "Foreign Business";
constexpr std::string_view GGenreForeignChildrensBooks = "Foreign Children's Books";
constexpr std::string_view GGenreForeignClassics = "Foreign Classics";
constexpr std::string_view GGenreForeignComputers = "Foreign Computers";
constexpr std::string_view GGenreForeignDetectives = "Foreign Detectives";
constexpr std::string_view GGenreForeignDrama = "Foreign Drama";
constexpr std::string_view GGenreForeignEssays = "Foreign Essays";
constexpr std::string_view GGenreForeignFairyTales = "Foreign Fairy Tales";
constexpr std::string_view GGenreForeignFantasy = "Foreign Fantasy";
constexpr std::string_view GGenreForeignHumor = "Foreign Humor";
constexpr std::string_view GGenreForeignLanguages = "Foreign Languages";
constexpr std::string_view GGenreForeignLiterature = "Foreign Literature";
constexpr std::string_view GGenreForeignPoetry = "Foreign Poetry";
constexpr std::string_view GGenreForeignProse = "Foreign Prose";
constexpr std::string_view GGenreForeignRomance = "Foreign Romance";
constexpr std::string_view GGenreForeignScienceFiction = "Foreign Science Fiction";
constexpr std::string_view GGenreFreeVerse = "Free Verse";
constexpr std::string_view GGenreFriendship = "Friendship";
constexpr std::string_view GGenreGameInspiredProse = "Game-Inspired Prose";
constexpr std::string_view GGenreGarden = "Garden";
constexpr std::string_view GGenreGeneral = "General";
constexpr std::string_view GGenreGeography = "Geography";
constexpr std::string_view GGenreGothicFiction = "Gothic Fiction";
constexpr std::string_view GGenreGuidebooks = "Guidebooks";
constexpr std::string_view GGenreHardBoiled = "Hard-Boiled";
constexpr std::string_view GGenreHardware = "Hardware";
constexpr std::string_view GGenreHealth = "Health";
constexpr std::string_view GGenreHealthPsychology = "Health Psychology";
constexpr std::string_view GGenreHeroic = "Heroic";
constexpr std::string_view GGenreHeroicFantasy = "Heroic Fantasy";
constexpr std::string_view GGenreHigherEducation = "Higher Education";
constexpr std::string_view GGenreHinduism = "Hinduism";
constexpr std::string_view GGenreHistoricalDetectives = "Historical Detectives";
constexpr std::string_view GGenreHistoricalFantasy = "Historical Fantasy";
constexpr std::string_view GGenreHistoricalFiction = "Historical Fiction";
constexpr std::string_view GGenreHistoricalProse = "Historical Prose";
constexpr std::string_view GGenreHistoricalRomance = "Historical Romance";
constexpr std::string_view GGenreHistory = "History";
constexpr std::string_view GGenreHistoryPhilosophyofScience = "History & Philosophy of Science";
constexpr std::string_view GGenreHobbiesCrafts = "Hobbies & Crafts";
constexpr std::string_view GGenreHomeFamily = "Home & Family";
constexpr std::string_view GGenreHorror = "Horror";
constexpr std::string_view GGenreHorrorMystic = "Horror & Mystic";
constexpr std::string_view GGenreHumor = "Humor";
constexpr std::string_view GGenreHumorousFantasy = "Humorous Fantasy";
constexpr std::string_view GGenreHumorousProse = "Humorous Prose";
constexpr std::string_view GGenreHumorousSF = "Humorous SF";
constexpr std::string_view GGenreHumorousVerses = "Humorous Verses";
constexpr std::string_view GGenreHuntingFishing = "Hunting & Fishing";
constexpr std::string_view GGenreIndians = "Indians";
constexpr std::string_view GGenreIndustries = "Industries";
constexpr std::string_view GGenreInternet = "Internet";
constexpr std::string_view GGenreIronicalDetectives = "Ironical Detectives";
constexpr std::string_view GGenreIsekaiPortalFantasy = "Isekai / Portal Fantasy";
constexpr std::string_view GGenreIslam = "Islam";
constexpr std::string_view GGenreJudaism = "Judaism";
constexpr std::string_view GGenreJurisprudence = "Jurisprudence";
constexpr std::string_view GGenreLegalThrillers = "Legal Thrillers";
constexpr std::string_view GGenreLightFiction = "Light Fiction";
constexpr std::string_view GGenreLimericks = "Limericks";
constexpr std::string_view GGenreLinguistics = "Linguistics";
constexpr std::string_view GGenreLiteraryCriticism = "Literary Criticism";
constexpr std::string_view GGenreLiterature = "Literature";
constexpr std::string_view GGenreLitRPG = "LitRPG";
constexpr std::string_view GGenreMagic = "Magic";
constexpr std::string_view GGenreMagicSchool = "Magic School";
constexpr std::string_view GGenreManagement = "Management";
constexpr std::string_view GGenreManiacs = "Maniacs";
constexpr std::string_view GGenreMaritimeFiction = "Maritime Fiction";
constexpr std::string_view GGenreMarketing = "Marketing";
constexpr std::string_view GGenreMartialArts = "Martial Arts";
constexpr std::string_view GGenreMathematics = "Mathematics";
constexpr std::string_view GGenreMedicalThrillers = "Medical Thrillers";
constexpr std::string_view GGenreMedicine = "Medicine";
constexpr std::string_view GGenreMilitary = "Military";
constexpr std::string_view GGenreMilitaryAdventure = "Military Adventure";
constexpr std::string_view GGenreMilitaryHistory = "Military History";
constexpr std::string_view GGenreMilitaryNonFiction = "Military Non-Fiction";
constexpr std::string_view GGenreMilitaryProse = "Military Prose";
constexpr std::string_view GGenreMilitaryTechnology = "Military Technology";
constexpr std::string_view GGenreModernAdventure = "Modern Adventure";
constexpr std::string_view GGenreModernFairyTales = "Modern Fairy Tales";
constexpr std::string_view GGenreModernPoetry = "Modern Poetry";
constexpr std::string_view GGenreModernRussianPoetry = "Modern Russian Poetry";
constexpr std::string_view GGenreMuseumsCollections = "Museums & Collections";
constexpr std::string_view GGenreMusic = "Music";
constexpr std::string_view GGenreMystery = "Mystery";
constexpr std::string_view GGenreMysteryThriller = "Mystery Thriller";
constexpr std::string_view GGenreMysticSupernatural = "Mystic & Supernatural";
constexpr std::string_view GGenreMysticSF = "Mystic SF";
constexpr std::string_view GGenreMythicalCreatures = "Mythical Creatures";
constexpr std::string_view GGenreMythsLegendsEpics = "Myths, Legends & Epics";
constexpr std::string_view GGenreNatureAnimals = "Nature & Animals";
constexpr std::string_view GGenreNatureWildlife = "Nature & Wildlife";
constexpr std::string_view GGenreNetworkLiterature = "Network Literature";
constexpr std::string_view GGenreNeurobiology = "Neurobiology";
constexpr std::string_view GGenreNewAuthors = "New Authors";
constexpr std::string_view GGenreNonFiction = "Non-Fiction";
constexpr std::string_view GGenreNotesEssays = "Notes & Essays";
constexpr std::string_view GGenreNovel = "Novel";
constexpr std::string_view GGenreNovella = "Novella";
constexpr std::string_view GGenreNutritionHealth = "Nutrition & Health";
constexpr std::string_view GGenreOccult = "Occult";
constexpr std::string_view GGenreOccultHorror = "Occult Horror";
constexpr std::string_view GGenreOfficeAdministration = "Office & Administration";
constexpr std::string_view GGenreOldEastern = "Old Eastern";
constexpr std::string_view GGenreOldRussian = "Old Russian";
constexpr std::string_view GGenreOrganizationalBehavior = "Organizational Behavior";
constexpr std::string_view GGenreOrientalStudies = "Oriental Studies";
constexpr std::string_view GGenreOrthodoxChristianity = "Orthodox Christianity";
constexpr std::string_view GGenreOSNetworking = "OS & Networking";
constexpr std::string_view GGenreOther = "Other";
constexpr std::string_view GGenrePaganism = "Paganism";
constexpr std::string_view GGenrePaintingVisualArts = "Painting & Visual Arts";
constexpr std::string_view GGenreParanormalFiction = "Paranormal Fiction";
constexpr std::string_view GGenreParenting = "Parenting";
constexpr std::string_view GGenreParentingUpbringing = "Parenting & Upbringing";
constexpr std::string_view GGenreParodySatire = "Parody & Satire";
constexpr std::string_view GGenrePedagogy = "Pedagogy";
constexpr std::string_view GGenrePeopleSociety = "People & Society";
constexpr std::string_view GGenrePerformingArts = "Performing Arts";
constexpr std::string_view GGenrePeriodicals = "Periodicals";
constexpr std::string_view GGenrePersonalFinance = "Personal Finance";
constexpr std::string_view GGenrePets = "Pets";
constexpr std::string_view GGenrePhilology = "Philology";
constexpr std::string_view GGenrePhilosophy = "Philosophy";
constexpr std::string_view GGenrePhysics = "Physics";
constexpr std::string_view GGenrePoetry = "Poetry";
constexpr std::string_view GGenrePoliceStories = "Police Stories";
constexpr std::string_view GGenrePoliceThrillers = "Police Thrillers";
constexpr std::string_view GGenrePoliticalDetectives = "Political Detectives";
constexpr std::string_view GGenrePoliticalFiction = "Political Fiction";
constexpr std::string_view GGenrePoliticalScience = "Political Science";
constexpr std::string_view GGenrePolitics = "Politics";
constexpr std::string_view GGenrePoliticsSociety = "Politics & Society";
constexpr std::string_view GGenrePopularBusiness = "Popular Business";
constexpr std::string_view GGenrePopularPsychology = "Popular Psychology";
constexpr std::string_view GGenrePopularScience = "Popular Science";
constexpr std::string_view GGenrePortalFantasy = "Portal Fantasy";
constexpr std::string_view GGenrePostApocalyptic = "Post-Apocalyptic";
constexpr std::string_view GGenrePregnancyParenting = "Pregnancy & Parenting";
constexpr std::string_view GGenreProgramming = "Programming";
constexpr std::string_view GGenreProse = "Prose";
constexpr std::string_view GGenreProtestantism = "Protestantism";
constexpr std::string_view GGenreProverbs = "Proverbs";
constexpr std::string_view GGenrePsychologicalThriller = "Psychological Thriller";
constexpr std::string_view GGenrePsychology = "Psychology";
constexpr std::string_view GGenrePublicSpeaking = "Public Speaking";
constexpr std::string_view GGenrePublicism = "Publicism";
constexpr std::string_view GGenreRadioElectronics = "Radio & Electronics";
constexpr std::string_view GGenreRealEstate = "Real Estate";
constexpr std::string_view GGenreReference = "Reference";
constexpr std::string_view GGenreReligion = "Religion";
constexpr std::string_view GGenreReligiousFiction = "Religious Fiction";
constexpr std::string_view GGenreReligiousLiterature = "Religious Literature";
constexpr std::string_view GGenreReligiousStudies = "Religious Studies";
constexpr std::string_view GGenreRiddles = "Riddles";
constexpr std::string_view GGenreRomance = "Romance";
constexpr std::string_view GGenreRomanceScienceFiction = "Romance Science Fiction";
constexpr std::string_view GGenreRomanticSuspense = "Romantic Suspense";
constexpr std::string_view GGenreRussianClassicLiterature = "Russian Classic Literature";
constexpr std::string_view GGenreRussianClassicProse = "Russian Classic Prose";
constexpr std::string_view GGenreRussianClassicalPoetry = "Russian Classical Poetry";
constexpr std::string_view GGenreRussianFairyTales = "Russian Fairy Tales";
constexpr std::string_view GGenreRussianFantasy = "Russian Fantasy";
constexpr std::string_view GGenreRussianLiterature = "Russian Literature";
constexpr std::string_view GGenreRussianPoetry = "Russian Poetry";
constexpr std::string_view GGenreRussianScienceFiction = "Russian Science Fiction";
constexpr std::string_view GGenreSagas = "Sagas";
constexpr std::string_view GGenreSamizdat = "Samizdat";
constexpr std::string_view GGenreSatireHumor = "Satire & Humor";
constexpr std::string_view GGenreScenarios = "Scenarios";
constexpr std::string_view GGenreSchoolEducation = "School Education";
constexpr std::string_view GGenreSchoolStories = "School Stories";
constexpr std::string_view GGenreScience = "Science";
constexpr std::string_view GGenreScienceFantasy = "Science Fantasy";
constexpr std::string_view GGenreScienceFiction = "Science Fiction";
constexpr std::string_view GGenreScienceFictionRomance = "Science Fiction Romance";
constexpr std::string_view GGenreScientificTheories = "Scientific Theories";
constexpr std::string_view GGenreScreenplays = "Screenplays";
constexpr std::string_view GGenreSecondaryEducation = "Secondary Education";
constexpr std::string_view GGenreSelfHelpHealth = "Self-Help & Health";
constexpr std::string_view GGenreSelfImprovement = "Self-Improvement";
constexpr std::string_view GGenreSentimentalProse = "Sentimental Prose";
constexpr std::string_view GGenreShortRomance = "Short Romance";
constexpr std::string_view GGenreShortStories = "Short Stories";
constexpr std::string_view GGenreSketches = "Sketches";
constexpr std::string_view GGenreSlavicFantasy = "Slavic Fantasy";
constexpr std::string_view GGenreSliceofLife = "Slice of Life";
constexpr std::string_view GGenreSmallBusiness = "Small Business";
constexpr std::string_view GGenreSocialPsychology = "Social Psychology";
constexpr std::string_view GGenreSocialStudies = "Social Studies";
constexpr std::string_view GGenreSocialWork = "Social Work";
constexpr std::string_view GGenreSocialPhilosophical = "Social-Philosophical";
constexpr std::string_view GGenreSociology = "Sociology";
constexpr std::string_view GGenreSoftware = "Software";
constexpr std::string_view GGenreSongLyricsPoetry = "Song Lyrics & Poetry";
constexpr std::string_view GGenreSongPoetry = "Song Poetry";
constexpr std::string_view GGenreSovietClassicProse = "Soviet Classic Prose";
constexpr std::string_view GGenreSovietLiterature = "Soviet Literature";
constexpr std::string_view GGenreSovietEraDetectives = "Soviet-Era Detectives";
constexpr std::string_view GGenreSpace = "Space";
constexpr std::string_view GGenreSpaceFiction = "Space Fiction";
constexpr std::string_view GGenreSpaceOpera = "Space Opera";
constexpr std::string_view GGenreSpirituality = "Spirituality";
constexpr std::string_view GGenreSports = "Sports";
constexpr std::string_view GGenreSteampunk = "Steampunk";
constexpr std::string_view GGenreStudyGuides = "Study Guides";
constexpr std::string_view GGenreTechnical = "Technical";
constexpr std::string_view GGenreTechnoThriller = "Techno-Thriller";
constexpr std::string_view GGenreTeenFiction = "Teen Fiction";
constexpr std::string_view GGenreTeenLiterature = "Teen Literature";
constexpr std::string_view GGenreTeenScienceFiction = "Teen Science Fiction";
constexpr std::string_view GGenreTextbooks = "Textbooks";
constexpr std::string_view GGenreTheatre = "Theatre";
constexpr std::string_view GGenreThrillers = "Thrillers";
constexpr std::string_view GGenreTimeTravelFantasy = "Time-Travel Fantasy";
constexpr std::string_view GGenreTimeTravelFiction = "Time-Travel Fiction";
constexpr std::string_view GGenreTragedy = "Tragedy";
constexpr std::string_view GGenreTransportationTechnology = "Transportation & Technology";
constexpr std::string_view GGenreTravel = "Travel";
constexpr std::string_view GGenreTravelGeography = "Travel & Geography";
constexpr std::string_view GGenreTravelGuides = "Travel Guides";
constexpr std::string_view GGenreTravelNotes = "Travel Notes";
constexpr std::string_view GGenreUnfinishedWorks = "Unfinished Works";
constexpr std::string_view GGenreUrbanFantasy = "Urban Fantasy";
constexpr std::string_view GGenreUtopianFiction = "Utopian Fiction";
constexpr std::string_view GGenreVaudeville = "Vaudeville";
constexpr std::string_view GGenreVeterinaryMedicine = "Veterinary Medicine";
constexpr std::string_view GGenreVisualArts = "Visual Arts";
constexpr std::string_view GGenreVisualPoetry = "Visual Poetry";
constexpr std::string_view GGenreWarLiterature = "War Literature";
constexpr std::string_view GGenreWestern = "Western";
constexpr std::string_view GGenreWomensFiction = "Women's Fiction";
constexpr std::string_view GGenreWordplay = "Wordplay";
constexpr std::string_view GGenreWorldArtsCulture = "World Arts & Culture";
constexpr std::string_view GGenreWorldLiterature = "World Literature";
constexpr std::string_view GGenreWritersBiographies = "Writers' Biographies";
constexpr std::string_view GGenreWriting = "Writing";
constexpr std::string_view GGenreWuxia = "Wuxia";
constexpr std::string_view GGenreYoungAdult = "Young Adult";
constexpr std::string_view GGenreZoology = "Zoology";

// clang-format off
const std::unordered_map<std::string_view, std::string_view> GGenreNames{
    {"19_vek", GGenre19thCenturyLiterature},

    {"20_vek", GGenre20thCenturyLiterature},

    {"prose_abs", GGenreAbsurdistProse},

    {"accounting",     GGenreAccounting},
    {"biz_accounting", GGenreAccounting},

    {"Боевая фантастика", GGenreActionFantasy},
    {"Боевое фэнтези",    GGenreActionFantasy},
    {"fantasy_action",    GGenreActionFantasy},
    {"fantasy_fight",     GGenreActionFantasy},

    {"Экшн (action)", GGenreActionFiction},
    {"Action",        GGenreActionFiction},
    {"action",        GGenreActionFiction},

    {"boevaya_fantastika", GGenreActionSF},
    {"sf_action",          GGenreActionSF},

    {"adult", GGenreAdultFiction},

    {"Приключения",          GGenreAdventure},
    {"adv_all",              GGenreAdventure},
    {"adventure",            GGenreAdventure},
    {"Adventure",            GGenreAdventure},
    {"foreign_adventure",    GGenreAdventure},
    {"historical_adventure", GGenreAdventure},
    {"priklucheniya",        GGenreAdventure},
    {"priklucheniya_knigi",  GGenreAdventure},

    {"Приключенческое фэнтези", GGenreAdventureFantasy},
    {"adventure_fantasy",       GGenreAdventureFantasy},

    {"klassika_priklyuchenij", GGenreAdventureLiterature},
    {"literature_adv",         GGenreAdventureLiterature},
    {"literature_men_advent",  GGenreAdventureLiterature},

    {"adv_story", GGenreAdventureStories},

    {"сельское хозяйство", GGenreAgriculture},

    {"adv_history_avant", GGenreAlternativeHistory},
    {"back_to_ussr",      GGenreAlternativeHistory},
    {"sf_history",        GGenreAlternativeHistory},
    {"sf_history_avant",  GGenreAlternativeHistory},

    {"fantasy_alt_hist", GGenreAlternativeHistoryFantasy},

    {"Альтернативная медицина",  GGenreAlternativeMedicine},
    {"health_alt_medicine",      GGenreAlternativeMedicine},
    {"sci_medicine_alternative", GGenreAlternativeMedicine},

    {"American First Novelists", GGenreAmericanLiterature},
    {"literature_usa",           GGenreAmericanLiterature},

    {"history_ancient", GGenreAncientHistory},

    {"humor_anecdote", GGenreAnecdotes},

    {"child_adv_animal", GGenreAnimalAdventure},

    {"child_det_animal_detectives", GGenreAnimalDetectives},

    {"boyar_anime", GGenreAnimestyleFiction},

    {"compilation",         GGenreAnthology},
    {"literature_antology", GGenreAnthology},

    {"evoljuciya_i_antropologiya", GGenreAnthropology},

    {"antique_ant", GGenreAntique},

    {"antichnaya",      GGenreAntiqueLiterature},
    {"antique",         GGenreAntiqueLiterature},
    {"foreign_antique", GGenreAntiqueLiterature},

    {"aphorism_quote", GGenreAphorisms},
    {"aphorisms",      GGenreAphorisms},

    {"sci_archeology",      GGenreArchaeology},
    {"science_archaeology", GGenreArchaeology},

    {"architecture",      GGenreArchitecture},
    {"architecture_book", GGenreArchitecture},

    {"art", GGenreArt},

    {"design", GGenreArtDesign},

    {"art_criticism", GGenreArtCriticism},

    {"asian_fantasy", GGenreAsianFantasy},

    {"astrology", GGenreAstrology},

    {"kosmos_i_vselennaya", GGenreAstronomyCosmology},
    {"sci_cosmos",          GGenreAstronomyCosmology},

    {"auto_regulations", GGenreAutomotiveRegulations},

    {"sci_biochem", GGenreBiochemistry},

    {"biography", GGenreBiography},
    {"Biography", GGenreBiography},

    {"Биографии и мемуары",             GGenreBiographyMemoirs},
    {"Autobiografia",                   GGenreBiographyMemoirs},
    {"biogr_arts",                      GGenreBiographyMemoirs},
    {"biogr_historical",                GGenreBiographyMemoirs},
    {"biogr_leaders",                   GGenreBiographyMemoirs},
    {"biogr_professionals",             GGenreBiographyMemoirs},
    {"biogr_sports",                    GGenreBiographyMemoirs},
    {"biogr_travel",                    GGenreBiographyMemoirs},
    {"biografii_i_memuary",             GGenreBiographyMemoirs},
    {"Biography & Autobiography",       GGenreBiographyMemoirs},
    {"biz_beogr",                       GGenreBiographyMemoirs},
    {"nonf_biography",                  GGenreBiographyMemoirs},
    {"nonf_biography_celebrities",      GGenreBiographyMemoirs},
    {"nonf_biography_historical",       GGenreBiographyMemoirs},
    {"nonf_biography_military_figures", GGenreBiographyMemoirs},

    {"sci_biology",    GGenreBiology},
    {"science_biolog", GGenreBiology},

    {"sci_biophys", GGenreBiophysics},

    {"sci_botany", GGenreBotany},

    {"literature_british", GGenreBritishLiterature},

    {"rel_boddizm",       GGenreBuddhism},
    {"religion_budda",    GGenreBuddhism},
    {"religion_buddhism", GGenreBuddhism},

    {"auto_business", GGenreBusiness},
    {"business",      GGenreBusiness},
    {"sci_business",  GGenreBusiness},
    {"trade",         GGenreBusiness},

    {"biz_life", GGenreBusinessLifestyle},

    {"biz_management", GGenreBusinessManagement},

    {"job_hunting", GGenreCareerJobSearch},

    {"religion_catholicism", GGenreCatholicism},

    {"sci_chem",     GGenreChemistry},
    {"sci_orgchem",  GGenreChemistry},
    {"sci_physchem", GGenreChemistry},

    {"psy_childs", GGenreChildPsychology},

    {"Детская литература", GGenreChildrens},
    {"child_4",            GGenreChildrens},
    {"child_9",            GGenreChildrens},
    {"child_all",          GGenreChildrens},
    {"child_people",       GGenreChildrens},
    {"children",           GGenreChildrens},
    {"foreign_children",   GGenreChildrens},

    {"child_adv", GGenreChildrensAdventures},

    {"child_characters", GGenreChildrensCharacters},

    {"child_classical", GGenreChildrensClassics},

    {"child_det",                     GGenreChildrensDetectives},
    {"child_det_children",            GGenreChildrensDetectives},
    {"child_det_children_detectives", GGenreChildrensDetectives},
    {"child_det_other",               GGenreChildrensDetectives},
    {"detskie_detektivy",             GGenreChildrensDetectives},

    {"child_sf_fantasy", GGenreChildrensFantasy},

    {"child_folklore", GGenreChildrensFolklore},

    {"child_prose_history", GGenreChildrensHistoricalProse},

    {"child_history", GGenreChildrensHistory},
    {"teens_history", GGenreChildrensHistory},

    {"child_prose_humor", GGenreChildrensHumor},

    {"child_prose",          GGenreChildrensProse},
    {"child_prose_romantic", GGenreChildrensProse},
    {"detakaya_proza",       GGenreChildrensProse},

    {"child_sf",        GGenreChildrensScienceFiction},
    {"child_sf_horror", GGenreChildrensScienceFiction},
    {"child_sf_space",  GGenreChildrensScienceFiction},

    {"child_verse", GGenreChildrensVerses},

    {"chris_edu", GGenreChristianEducation},

    {"chris_fiction", GGenreChristianFiction},

    {"Христианство",          GGenreChristianity},
    {"religion_christianity", GGenreChristianity},

    {"cine", GGenreCinema},

    {"cinema_theatre",     GGenreCinemaTheatre},
    {"kinematograf_teatr", GGenreCinemaTheatre},

    {"klassicheskie",       GGenreClassicLiterature},
    {"literature_classics", GGenreClassicLiterature},

    {"Классическая проза", GGenreClassicProse},
    {"prose_classic",      GGenreClassicProse},

    {"det_classic", GGenreClassicalDetectives},

    {"poetry_classical",     GGenreClassicalPoetry},
    {"poetry_for_classical", GGenreClassicalPoetry},

    {"home_collecting", GGenreCollecting},

    {"collection", GGenreCollection},

    {"Комедия", GGenreComedy},
    {"Comedy",  GGenreComedy},
    {"comedy",  GGenreComedy},

    {"comics", GGenreComicsGraphicNovels},

    {"tbg_computers", GGenreComputerEducation},

    {"comp_all",             GGenreComputersInternet},
    {"computer_translation", GGenreComputersInternet},
    {"computers",            GGenreComputersInternet},

    {"outdoors_conservation", GGenreConservation},

    {"sci_build", GGenreConstructionArchitecture},

    {"sovremennye",  GGenreContemporaryFiction},
    {"sovremennyye", GGenreContemporaryFiction},

    {"foreign_contemporary",                  GGenreContemporaryForeignFiction},
    {"knigi_sovremennaya_proza_zarubezhnaya", GGenreContemporaryForeignFiction},

    {"Современная Проза",  GGenreContemporaryProse},
    {"Современная проза",  GGenreContemporaryProse},
    {"современная проза",  GGenreContemporaryProse},
    {"prose_contemporary", GGenreContemporaryProse},

    {"love_contemporary",    GGenreContemporaryRomance},
    {"romance_contemporary", GGenreContemporaryRomance},

    {"russian_contemporary", GGenreContemporaryRussianFiction},

    {"cooking",      GGenreCooking},
    {"home_cooking", GGenreCooking},

    {"prose_counter", GGenreCounterculture},

    {"det_cozy", GGenreCozyMysteries},

    {"Assassins",        GGenreCrime},
    {"det_crime",        GGenreCrime},
    {"Murder",           GGenreCrime},
    {"Murder - General", GGenreCrime},
    {"Organized Crime",  GGenreCrime},

    {"Murder - England - Wiltshire - History - 19th century", GGenreCrimeNonFiction},
    {"nonfiction_crime",                                      GGenreCrimeNonFiction},
    {"True Crime",                                            GGenreCrimeNonFiction},

    {"kriminologiya", GGenreCriminology},

    {"nonf_criticism", GGenreCriticism},

    {"kulturologiya", GGenreCulturalStudies},
    {"sci_culture",   GGenreCulturalStudies},

    {"cyberpunk",     GGenreCyberpunk},
    {"sf_cyber_punk", GGenreCyberpunk},
    {"sf_cyberpunk",  GGenreCyberpunk},

    {"music_dancing", GGenreDance},

    {"темная фантастика", GGenreDarkFantasy},
    {"dark_fantasy",      GGenreDarkFantasy},
    {"fantasy_dark",      GGenreDarkFantasy},

    {"comp_db", GGenreDatabases},

    {"detective_science_fiction", GGenreDetective},
    {"sf_detective",              GGenreDetective},

    {"det_action", GGenreDetectiveAction},

    {"love_detective", GGenreDetectiveRomance},

    {"Детектив",                      GGenreDetectives},
    {"det_all",                       GGenreDetectives},
    {"det_lady",                      GGenreDetectives},
    {"det_rus",                       GGenreDetectives},
    {"detective",                     GGenreDetectives},
    {"Detective",                     GGenreDetectives},
    {"Detectives",                    GGenreDetectives},
    {"Detectives - England - London", GGenreDetectives},
    {"detektive",                     GGenreDetectives},
    {"Maigret",                       GGenreDetectives},
    {"Mystery & Detective",           GGenreDetectives},
    {"Mystery & Detective Fiction",   GGenreDetectives},

    {"ref_dict", GGenreDictionaries},

    {"dissident", GGenreDissidentLiterature},

    {"home_diy", GGenreDoItYourself},

    {"dragon_fantasy", GGenreDragonFantasy},
    {"drakony",        GGenreDragonFantasy},

    {"Драма",            GGenreDrama},
    {"dorama",           GGenreDrama},
    {"drama",            GGenreDrama},
    {"Drama",            GGenreDrama},
    {"literature_drama", GGenreDrama},

    {"dramaturgy",     GGenreDramaturgy},
    {"dramaturgy_all", GGenreDramaturgy},

    {"Антиутопия", GGenreDystopian},
    {"dystopia",   GGenreDystopian},
    {"Dystopian",  GGenreDystopian},
    {"dystopian",  GGenreDystopian},

    {"science_earth", GGenreEarthSciences},

    {"poetry_east", GGenreEasternPoetry},

    {"religion_east", GGenreEasternReligions},

    {"устойчивое развитие", GGenreEcology},
    {"экология",            GGenreEcology},
    {"sci_ecology",         GGenreEcology},

    {"Экономика",      GGenreEconomics},
    {"экономика дара", GGenreEconomics},
    {"biz_economics",  GGenreEconomics},
    {"economics",      GGenreEconomics},
    {"Economics",      GGenreEconomics},
    {"economics_ref",  GGenreEconomics},
    {"global_economy", GGenreEconomics},
    {"sci_economy",    GGenreEconomics},

    {"foreign_edu",                             GGenreEducation},
    {"nonfiction_edu",                          GGenreEducation},
    {"zarubezhnaya_obrazovatelnaya_literatura", GGenreEducation},

    {"child_education",                    GGenreEducational},
    {"detskaya_pazvivayushaya_literatura", GGenreEducational},

    {"ref_encyc", GGenreEncyclopedias},

    {"home_entertain", GGenreEntertaining},

    {"entert_humor", GGenreEntertainmentHumor},

    {"epic",       GGenreEpic},
    {"prose_epic", GGenreEpic},
    {"sf_epic",    GGenreEpic},

    {"epic_fantasy", GGenreEpicFantasy},
    {"fantasy_epic", GGenreEpicFantasy},

    {"epic_poetry", GGenreEpicPoetry},

    {"epistolary_fiction", GGenreEpistolaryFiction},

    {"Эротическое фэнтези", GGenreEroticFantasy},
    {"erotic_fantasy",      GGenreEroticFantasy},
    {"fantasy_erotika",     GGenreEroticFantasy},

    {"love_hard", GGenreEroticRomance},

    {"literature_erotica", GGenreErotica},
    {"love_erotica",       GGenreErotica},

    {"health_sex",        GGenreEroticaSex},
    {"home_sex",          GGenreEroticaSex},
    {"sexual_perversion", GGenreEroticaSex},

    {"Эзотерика",          GGenreEsoterics},
    {"palmistry",          GGenreEsoterics},
    {"religion_esoterics", GGenreEsoterics},

    {"Шпионский Детектив",  GGenreEspionage},
    {"det_espionage",       GGenreEspionage},
    {"Espionage",           GGenreEspionage},
    {"Fiction - Espionage", GGenreEspionage},

    {"essay",            GGenreEssays},
    {"essays",           GGenreEssays},
    {"literature_essay", GGenreEssays},

    {"antique_european", GGenreEuropeanAntique},

    {"everyday_fantasy", GGenreEverydayFantasy},

    {"Эксперимент", GGenreExperimentalFiction},

    {"experimental_poetry", GGenreExperimentalPoetry},

    {"prose_neformatny", GGenreExperimentalProse},

    {"extravaganza", GGenreExtravaganza},

    {"fable", GGenreFables},

    {"Казки",            GGenreFairyTales},
    {"Сказка",           GGenreFairyTales},
    {"child_tale",       GGenreFairyTales},
    {"fairy_tale",       GGenreFairyTales},
    {"literature_fairy", GGenreFairyTales},
    {"skazki",           GGenreFairyTales},

    {"family",           GGenreFamily},
    {"family_relations", GGenreFamily},

    {"family_health", GGenreFamilyHealth},

    {"fan_translation", GGenreFanTranslation},

    {"Ангст",             GGenreFanfiction},
    {"Даркфик",           GGenreFanfiction},
    {"Джен",              GGenreFanfiction},
    {"Первый раз",        GGenreFanfiction},
    {"Пропущенная сцена", GGenreFanfiction},
    {"фанфик",            GGenreFanfiction},
    {"Фанфик",            GGenreFanfiction},
    {"Флафф",             GGenreFanfiction},
    {"Angst",             GGenreFanfiction},
    {"AU",                GGenreFanfiction},
    {"Crossover",         GGenreFanfiction},
    {"Darkfic",           GGenreFanfiction},
    {"fanficion",         GGenreFanfiction},
    {"fanfiction",        GGenreFanfiction},
    {"femslash",          GGenreFanfiction},
    {"Hurt/comfort",      GGenreFanfiction},
    {"POV",               GGenreFanfiction},
    {"PWP",               GGenreFanfiction},
    {"slash",             GGenreFanfiction},
    {"Songfic",           GGenreFanfiction},

    {"фантастический мир", GGenreFantasy},
    {"Фентези",            GGenreFantasy},
    {"Фентъзи",            GGenreFantasy},
    {"Фэнтези",            GGenreFantasy},
    {"фэнтези",            GGenreFantasy},
    {"f_fantasy",          GGenreFantasy},
    {"fairy_fantasy",      GGenreFantasy},
    {"fantasy",            GGenreFantasy},
    {"Fantasy",            GGenreFantasy},
    {"popadancy",          GGenreFantasy},
    {"popadanec",          GGenreFantasy},
    {"prose_magic",        GGenreFantasy},
    {"sf_fantasy",         GGenreFantasy},

    {"Фэнтези Юмор", GGenreFantasyHumor},

    {"detective_fentezi", GGenreFantasyDetective},
    {"fantasy_det",       GGenreFantasyDetective},

    {"Любовное фэнтези", GGenreFantasyRomance},
    {"love_fantasy",     GGenreFantasyRomance},
    {"romance_fantasy",  GGenreFantasyRomance},

    {"Boys & Men",        GGenreFiction},
    {"Fiction",           GGenreFiction},
    {"fiction",           GGenreFiction},
    {"Fiction - General", GGenreFiction},
    {"women_single",      GGenreFiction},

    {"banking", GGenreFinanceBanking},

    {"stock", GGenreFinanceInvesting},

    {"folk_songs", GGenreFolkSongs},

    {"folk_tale", GGenreFolkTales},

    {"folklor",            GGenreFolklore},
    {"folklore",           GGenreFolklore},
    {"folklore_all",       GGenreFolklore},
    {"nonfiction_folklor", GGenreFolklore},

    {"foreign_action", GGenreForeignActionFiction},

    {"foreign_business", GGenreForeignBusiness},

    {"detskie_knigi_zarubezhnye", GGenreForeignChildrensBooks},

    {"zarubezhnaya_klassika",   GGenreForeignClassics},
    {"zarubezhnaya_starinnaya", GGenreForeignClassics},

    {"foreign_comp", GGenreForeignComputers},

    {"foreign_detective",           GGenreForeignDetectives},
    {"knigi_detektivy_zarubezhnye", GGenreForeignDetectives},

    {"foreign_dramaturgy", GGenreForeignDrama},

    {"foreign_desc", GGenreForeignEssays},

    {"child_tale_foreign_writers", GGenreForeignFairyTales},

    {"foreign_fantasy",           GGenreForeignFantasy},
    {"knigi_fentezi_zarubezhnye", GGenreForeignFantasy},

    {"foreign_humor",    GGenreForeignHumor},
    {"zarubezhnyy_umor", GGenreForeignHumor},

    {"foreign_language", GGenreForeignLanguages},
    {"nemetckij_yazyk",  GGenreForeignLanguages},

    {"zarubezhnaya", GGenreForeignLiterature},
    {"zarubezhnye",  GGenreForeignLiterature},
    {"zarubezhnyye", GGenreForeignLiterature},

    {"foreign_poetry", GGenreForeignPoetry},

    {"foreign_novel", GGenreForeignProse},
    {"foreign_prose", GGenreForeignProse},
    {"foreign_story", GGenreForeignProse},

    {"foreign_love", GGenreForeignRomance},

    {"foreign_sf", GGenreForeignScienceFiction},

    {"vers_libre", GGenreFreeVerse},

    {"Дружба", GGenreFriendship},

    {"prose_game", GGenreGameInspiredProse},

    {"home_garden", GGenreGarden},

    {"General", GGenreGeneral},

    {"geography_book", GGenreGeography},
    {"sci_geo",        GGenreGeography},

    {"gothic_novel", GGenreGothicFiction},

    {"ref_guide", GGenreGuidebooks},

    {"det_hard", GGenreHardBoiled},

    {"comp_hard", GGenreHardware},

    {"health",       GGenreHealth},
    {"health_men",   GGenreHealth},
    {"health_rel",   GGenreHealth},
    {"health_women", GGenreHealth},
    {"home_health",  GGenreHealth},
    {"teens_health", GGenreHealth},

    {"health_psy", GGenreHealthPsychology},

    {"sf_heroic", GGenreHeroic},

    {"fantasy_heroic",       GGenreHeroicFantasy},
    {"geroicheskoe_fentezi", GGenreHeroicFantasy},
    {"heroic_fantasy",       GGenreHeroicFantasy},

    {"tbg_higher", GGenreHigherEducation},

    {"religion_hinduism", GGenreHinduism},

    {"исторический детектив",         GGenreHistoricalDetectives},
    {"det_history",                   GGenreHistoricalDetectives},
    {"knigi_detektivy_istoricheskie", GGenreHistoricalDetectives},

    {"historical_fantasy", GGenreHistoricalFantasy},

    {"historical_fiction",        GGenreHistoricalFiction},
    {"istoricheskaya",            GGenreHistoricalFiction},
    {"istoricheskaya_literatura", GGenreHistoricalFiction},
    {"literature_history",        GGenreHistoricalFiction},

    {"Историческая проза", GGenreHistoricalProse},
    {"prose_history",      GGenreHistoricalProse},

    {"Исторические любовные романы", GGenreHistoricalRomance},
    {"love_history",                 GGenreHistoricalRomance},
    {"romance_historical",           GGenreHistoricalRomance},

    {"19th century",          GGenreHistory},
    {"Исторические эпохи",    GGenreHistory},
    {"история",               GGenreHistory},
    {"История",               GGenreHistory},
    {"Історія",               GGenreHistory},
    {"adv_history",           GGenreHistory},
    {"ci_history",            GGenreHistory},
    {"equ_history",           GGenreHistory},
    {"History",               GGenreHistory},
    {"history_asia",          GGenreHistory},
    {"history_australia",     GGenreHistory},
    {"history_europe",        GGenreHistory},
    {"history_middle_east",   GGenreHistory},
    {"history_russia",        GGenreHistory},
    {"history_usa",           GGenreHistory},
    {"history_world",         GGenreHistory},
    {"History: World",        GGenreHistory},
    {"populyarnaya_istoriya", GGenreHistory},
    {"sci_history",           GGenreHistory},
    {"sci_history_20",        GGenreHistory},
    {"sci_history_21",        GGenreHistory},

    {"science_history_philosophy", GGenreHistoryPhilosophyofScience},

    {"home_crafts", GGenreHobbiesCrafts},

    {"foreign_home", GGenreHomeFamily},
    {"home",         GGenreHomeFamily},
    {"home_all",     GGenreHomeFamily},

    {"horror",          GGenreHorror},
    {"horror_fantasy",  GGenreHorror},
    {"horror_usa",      GGenreHorror},
    {"horror_vampires", GGenreHorror},
    {"vampire_book",    GGenreHorror},

    {"sf_horror", GGenreHorrorMystic},

    {"Юмор",          GGenreHumor},
    {"humor",         GGenreHumor},
    {"Humor",         GGenreHumor},
    {"humor_all",     GGenreHumor},
    {"humor_fantasy", GGenreHumor},
    {"sf_humor",      GGenreHumor},

    {"ironical_fantasy", GGenreHumorousFantasy},
    {"sf_fantasy_irony", GGenreHumorousFantasy},

    {"юмористическая проза",   GGenreHumorousProse},
    {"humor_prose",            GGenreHumorousProse},
    {"umoristicheskaya_proza", GGenreHumorousProse},

    {"юмористическая фантастика", GGenreHumorousSF},
    {"sf_humory",                 GGenreHumorousSF},
    {"sf_irony",                  GGenreHumorousSF},

    {"humor_verse", GGenreHumorousVerses},

    {"outdoors_hunt_fish", GGenreHuntingFishing},

    {"adv_indian", GGenreIndians},

    {"industries", GGenreIndustries},

    {"Интернет", GGenreInternet},
    {"comp_www", GGenreInternet},

    {"Иронический детектив", GGenreIronicalDetectives},
    {"det_irony",            GGenreIronicalDetectives},
    {"ironicheskie",         GGenreIronicalDetectives},

    {"попаданец", GGenreIsekaiPortalFantasy},
    {"Попаданцы", GGenreIsekaiPortalFantasy},

    {"religion_islam", GGenreIslam},

    {"religion_judaism", GGenreJudaism},

    {"Науково- практичний коментар", GGenreJurisprudence},
    {"nonfiction_law",               GGenreJurisprudence},
    {"professional_law",             GGenreJurisprudence},
    {"sci_juris",                    GGenreJurisprudence},

    {"thriller_legal", GGenreLegalThrillers},

    {"legkaya_proza", GGenreLightFiction},

    {"limerick", GGenreLimericks},

    {"языкознание",    GGenreLinguistics},
    {"sci_linguistic", GGenreLinguistics},

    {"Literary Criticism", GGenreLiteraryCriticism},
    {"literature_critic",  GGenreLiteraryCriticism},

    {"literature",       GGenreLiterature},
    {"literature_18",    GGenreLiterature},
    {"literature_19",    GGenreLiterature},
    {"literature_20",    GGenreLiterature},
    {"literature_books", GGenreLiterature},

    {"ЛитРПГ",     GGenreLitRPG},
    {"РеалРПГ",    GGenreLitRPG},
    {"litrpg",     GGenreLitRPG},
    {"LitRpg",     GGenreLitRPG},
    {"realrpg",    GGenreLitRPG},
    {"sf_litrp",   GGenreLitRPG},
    {"sf_litrpg",  GGenreLitRPG},
    {"sf_litRPG",  GGenreLitRPG},
    {"sf_realrpg", GGenreLitRPG},

    {"magician_book", GGenreMagic},

    {"magic_school", GGenreMagicSchool},

    {"management", GGenreManagement},

    {"det_maniac", GGenreManiacs},

    {"adv_maritime",   GGenreMaritimeFiction},
    {"literature_sea", GGenreMaritimeFiction},

    {"marketing", GGenreMarketing},

    {"military_arts", GGenreMartialArts},

    {"Математика", GGenreMathematics},
    {"sci_math",   GGenreMathematics},

    {"thriller_medical", GGenreMedicalThrillers},

    {"Medical",          GGenreMedicine},
    {"medicine",         GGenreMedicine},
    {"Physicians",       GGenreMedicine},
    {"sci_medicine",     GGenreMedicine},
    {"science_medicine", GGenreMedicine},

    {"military",                 GGenreMilitary},
    {"military_all",             GGenreMilitary},
    {"military_special",         GGenreMilitary},
    {"voennoe_delo_specsluzhby", GGenreMilitary},
    {"War & Military",           GGenreMilitary},

    {"adv_military", GGenreMilitaryAdventure},

    {"histor_military",          GGenreMilitaryHistory},
    {"history_military_science", GGenreMilitaryHistory},
    {"military_history",         GGenreMilitaryHistory},

    {"nonf_military", GGenreMilitaryNonFiction},

    {"prose_military", GGenreMilitaryProse},

    {"military_weapon", GGenreMilitaryTechnology},

    {"adv_modern", GGenreModernAdventure},

    {"modern_tale", GGenreModernFairyTales},

    {"poetry_for_modern", GGenreModernPoetry},
    {"poetry_modern",     GGenreModernPoetry},

    {"poetry_rus_modern", GGenreModernRussianPoetry},

    {"muzei_kollektsii", GGenreMuseumsCollections},

    {"about_musicians", GGenreMusic},
    {"music",           GGenreMusic},

    {"mystery", GGenreMystery},

    {"American Mystery & Suspense Fiction", GGenreMysteryThriller},
    {"Mysteries & Thrillers",               GGenreMysteryThriller},
    {"thriller_mystery",                    GGenreMysteryThriller},

    {"Мистика", GGenreMysticSupernatural},
    {"mistika", GGenreMysticSupernatural},

    {"sf_mystic", GGenreMysticSF},

    {"Мифические существа",     GGenreMythicalCreatures},
    {"фантастические существа", GGenreMythicalCreatures},

    {"antique_myths",     GGenreMythsLegendsEpics},
    {"mify_legendy_epos", GGenreMythsLegendsEpics},

    {"adv_animal",    GGenreNatureAnimals},
    {"child_animals", GGenreNatureAnimals},
    {"child_nature",  GGenreNatureAnimals},

    {"outdoors_fauna",          GGenreNatureWildlife},
    {"outdoors_nature_writing", GGenreNatureWildlife},

    {"network_literature",  GGenreNetworkLiterature},
    {"snetwork_literature", GGenreNetworkLiterature},

    {"nejrobiologiya", GGenreNeurobiology},

    {"beginning_authors", GGenreNewAuthors},

    {"Case studies",                        GGenreNonFiction},
    {"dokumentalnaya_literatura",           GGenreNonFiction},
    {"interview",                           GGenreNonFiction},
    {"nonf_all",                            GGenreNonFiction},
    {"nonfiction",                          GGenreNonFiction},
    {"nonfiction_spec_group",               GGenreNonFiction},
    {"nonfiction_true_accounts",            GGenreNonFiction},
    {"zarubezhnaya_prikladnaya_literatura", GGenreNonFiction},

    {"notes", GGenreNotesEssays},

    {"Романы",     GGenreNovel},
    {"long_story", GGenreNovel},
    {"novel",      GGenreNovel},
    {"roman",      GGenreNovel},

    {"great_story",  GGenreNovella},
    {"greate_story", GGenreNovella},

    {"health_nutrition", GGenreNutritionHealth},

    {"religion_occult", GGenreOccult},

    {"horror_occult", GGenreOccultHorror},

    {"paper_work", GGenreOfficeAdministration},

    {"antique_east",      GGenreOldEastern},
    {"drevnevostochnaya", GGenreOldEastern},

    {"antique_russian", GGenreOldRussian},

    {"org_behavior", GGenreOrganizationalBehavior},

    {"sci_oriental", GGenreOrientalStudies},

    {"Православие",        GGenreOrthodoxChristianity},
    {"chris_orthodoxy",    GGenreOrthodoxChristianity},
    {"chris_pravoslavie",  GGenreOrthodoxChristianity},
    {"religion_orthodoxy", GGenreOrthodoxChristianity},

    {"comp_osnet", GGenreOSNetworking},

    {"?",                 GGenreOther},
    {"105",               GGenreOther},
    {"1979",              GGenreOther},
    {"ю",                 GGenreOther},
    {"England",           GGenreOther},
    {"f",                 GGenreOther},
    {"Lang:fr",           GGenreOther},
    {"London",            GGenreOther},
    {"other",             GGenreOther},
    {"other_all",         GGenreOther},
    {"unrecognised",      GGenreOther},
    {"Whicher; Jonathan", GGenreOther},
    {"Wiltshire",         GGenreOther},

    {"religion_paganism", GGenrePaganism},

    {"painting", GGenrePaintingVisualArts},

    {"paranormal", GGenreParanormalFiction},

    {"family_parenting", GGenreParenting},

    {"upbringing_book", GGenreParentingUpbringing},

    {"Пародия", GGenreParodySatire},
    {"Стёб",    GGenreParodySatire},
    {"Parody",  GGenreParodySatire},

    {"pedagogy_book", GGenrePedagogy},
    {"sci_pedagogy",  GGenrePedagogy},

    {"people", GGenrePeopleSociety},

    {"performance", GGenrePerformingArts},

    {"Журналы",    GGenrePeriodicals},
    {"newspapers", GGenrePeriodicals},
    {"periodic",   GGenrePeriodicals},

    {"personal_finance", GGenrePersonalFinance},

    {"domashnie_zhivotnye", GGenrePets},
    {"home_pets",           GGenrePets},

    {"sci_philology", GGenrePhilology},

    {"Философия",             GGenrePhilosophy},
    {"nonfiction_philosophy", GGenrePhilosophy},
    {"sci_philosophy",        GGenrePhilosophy},

    {"Physicists",             GGenrePhysics},
    {"Physics",                GGenrePhysics},
    {"Relativity",             GGenrePhysics},
    {"Relativity (Physics)",   GGenrePhysics},
    {"sci_phys",               GGenrePhysics},
    {"Unified Field Theories", GGenrePhysics},

    {"поезія",            GGenrePoetry},
    {"поэзия",            GGenrePoetry},
    {"in_verse",          GGenrePoetry},
    {"literature_poetry", GGenrePoetry},
    {"poem",              GGenrePoetry},
    {"poetry",            GGenrePoetry},
    {"poetry_all",        GGenrePoetry},
    {"poeziya",           GGenrePoetry},

    {"det_police",      GGenrePoliceStories},
    {"Law Enforcement", GGenrePoliceStories},
    {"Policier",        GGenrePoliceStories},

    {"thriller_police", GGenrePoliceThrillers},

    {"det_political", GGenrePoliticalDetectives},

    {"literature_political", GGenrePoliticalFiction},

    {"sci_state", GGenrePoliticalScience},

    {"Political",    GGenrePolitics},
    {"sci_politics", GGenrePolitics},

    {"анархизм",            GGenrePoliticsSociety},
    {"анархия",             GGenrePoliticsSociety},
    {"геополитика",         GGenrePoliticsSociety},
    {"nonfiction_politics", GGenrePoliticsSociety},

    {"popular_business", GGenrePopularBusiness},

    {"sci_psychology_popular", GGenrePopularPsychology},

    {"nauchno_populyarnaya_literatura", GGenrePopularScience},
    {"sci_popular",                     GGenrePopularScience},

    {"popadantsy",                    GGenrePortalFantasy},
    {"popadantsy_v_magicheskie_miry", GGenrePortalFantasy},

    {"Постапокалиптика",   GGenrePostApocalyptic},
    {"postapocalyptic",    GGenrePostApocalyptic},
    {"sf_postapocalyptic", GGenrePostApocalyptic},

    {"family_pregnancy", GGenrePregnancyParenting},

    {"comp_programming", GGenreProgramming},

    {"Проза",      GGenreProse},
    {"narrative",  GGenreProse},
    {"proce",      GGenreProse},
    {"prose",      GGenreProse},
    {"Prose",      GGenreProse},
    {"prose_all",  GGenreProse},
    {"prose_root", GGenreProse},

    {"religion_protestantism", GGenreProtestantism},

    {"proverbs", GGenreProverbs},

    {"thriller_psychology", GGenrePsychologicalThriller},

    {"Психология",           GGenrePsychology},
    {"психология",           GGenrePsychology},
    {"foreign_psychology",   GGenrePsychology},
    {"obschaya_psihologiya", GGenrePsychology},
    {"psy_alassic",          GGenrePsychology},
    {"psy_generic",          GGenrePsychology},
    {"psy_personal",         GGenrePsychology},
    {"psy_sex_and_family",   GGenrePsychology},
    {"psy_theraphy",         GGenrePsychology},
    {"sci_psychology",       GGenrePsychology},
    {"science_psy",          GGenrePsychology},

    {"oratorskoye_iskusstvo", GGenrePublicSpeaking},

    {"публицистика",         GGenrePublicism},
    {"Публицистика",         GGenrePublicism},
    {"Публицистика: прочее", GGenrePublicism},
    {"foreign_publicism",    GGenrePublicism},
    {"nonf_publicism",       GGenrePublicism},

    {"sci_radio", GGenreRadioElectronics},

    {"Недвижимость", GGenreRealEstate},
    {"real_estate",  GGenreRealEstate},

    {"hand_book", GGenreReference},
    {"ref_all",   GGenreReference},
    {"ref_books", GGenreReference},
    {"ref_ref",   GGenreReference},
    {"reference", GGenreReference},

    {"Религия",          GGenreReligion},
    {"foreign_religion", GGenreReligion},
    {"religion",         GGenreReligion},
    {"religion_all",     GGenreReligion},
    {"religion_rel",     GGenreReligion},

    {"literature_religion", GGenreReligiousFiction},

    {"религиозная литература", GGenreReligiousLiterature},

    {"sci_religion", GGenreReligiousStudies},

    {"riddles", GGenreRiddles},

    {"Любовь/Ненависть",      GGenreRomance},
    {"Романтика",             GGenreRomance},
    {"романтика",             GGenreRomance},
    {"love",                  GGenreRomance},
    {"love_all",              GGenreRomance},
    {"lubov",                 GGenreRomance},
    {"romance",               GGenreRomance},
    {"Romance",               GGenreRomance},
    {"romance_multicultural", GGenreRomance},

    {"love_sf", GGenreRomanceScienceFiction},
    {"sf_love", GGenreRomanceScienceFiction},

    {"romance_romantic_suspense", GGenreRomanticSuspense},

    {"klassicheskaya_literatura_russkaya", GGenreRussianClassicLiterature},
    {"literature_rus_classsic",            GGenreRussianClassicLiterature},

    {"prose_rus_classic",  GGenreRussianClassicProse},
    {"prose_rus_classics", GGenreRussianClassicProse},

    {"poetry_rus_classical", GGenreRussianClassicalPoetry},

    {"child_tale_rus",             GGenreRussianFairyTales},
    {"child_tale_russian_writers", GGenreRussianFairyTales},

    {"fantasy_rus",     GGenreRussianFantasy},
    {"russian_fantasy", GGenreRussianFantasy},

    {"russian",             GGenreRussianLiterature},
    {"russkaya_literatura", GGenreRussianLiterature},

    {"russkaya_poeziya", GGenreRussianPoetry},

    {"sf_rus", GGenreRussianScienceFiction},

    {"saga",  GGenreSagas},
    {"sagas", GGenreSagas},

    {"samizdat", GGenreSamizdat},

    {"Black Humor (Literature)", GGenreSatireHumor},
    {"humor_satire",             GGenreSatireHumor},
    {"umori_i_satira",           GGenreSatireHumor},

    {"scenarios", GGenreScenarios},

    {"tbg_school", GGenreSchoolEducation},

    {"Учебные заведения", GGenreSchoolStories},

    {"biologiya_i_himiya",   GGenreScience},
    {"nauchnaya",            GGenreScience},
    {"sci_abstract",         GGenreScience},
    {"science",              GGenreScience},
    {"Science",              GGenreScience},
    {"Science & Technology", GGenreScience},
    {"this is science",      GGenreScience},

    {"sf_technofantasy", GGenreScienceFantasy},

    {"миры eve",               GGenreScienceFiction},
    {"научная фантастика",     GGenreScienceFiction},
    {"НФ",                     GGenreScienceFiction},
    {"современная фантастика", GGenreScienceFiction},
    {"Фантастика",             GGenreScienceFiction},
    {"klassika_fantastiki",    GGenreScienceFiction},
    {"nsf",                    GGenreScienceFiction},
    {"sci-fi",                 GGenreScienceFiction},
    {"Science Fiction",        GGenreScienceFiction},
    {"science_fiction",        GGenreScienceFiction},
    {"sf",                     GGenreScienceFiction},
    {"SF",                     GGenreScienceFiction},
    {"sf_all",                 GGenreScienceFiction},
    {"sf_etc",                 GGenreScienceFiction},
    {"sf_paleontological",     GGenreScienceFiction},

    {"romance_sf", GGenreScienceFictionRomance},

    {"sci_theories", GGenreScientificTheories},

    {"screenplays", GGenreScreenplays},

    {"tbg_secondary", GGenreSecondaryEducation},

    {"health_self_help", GGenreSelfHelpHealth},

    {"Самосовершенствование",         GGenreSelfImprovement},
    {"religion_self",                 GGenreSelfImprovement},
    {"samorazvitiye_lichnostny_rost", GGenreSelfImprovement},

    {"prose_sentimental", GGenreSentimentalProse},

    {"love_short", GGenreShortRomance},

    {"literature_short", GGenreShortStories},
    {"short_story",      GGenreShortStories},
    {"story",            GGenreShortStories},

    {"sketch", GGenreSketches},

    {"slavic_fantasy", GGenreSlavicFantasy},

    {"Повседневность",    GGenreSliceofLife},
    {"istorii_iz_zhizni", GGenreSliceofLife},

    {"small_business", GGenreSmallBusiness},

    {"psy_social", GGenreSocialPsychology},

    {"nonfiction_social_sci", GGenreSocialStudies},
    {"sci_social_studies",    GGenreSocialStudies},

    {"nonfiction_social_work", GGenreSocialWork},

    {"sf_social", GGenreSocialPhilosophical},

    {"nonfiction_sociology", GGenreSociology},
    {"sociology_book",       GGenreSociology},

    {"comp_soft", GGenreSoftware},

    {"lyrics", GGenreSongLyricsPoetry},

    {"song_poetry", GGenreSongPoetry},

    {"prose_su_classic",  GGenreSovietClassicProse},
    {"prose_su_classics", GGenreSovietClassicProse},

    {"literature_su_classics", GGenreSovietLiterature},
    {"su_publication",         GGenreSovietLiterature},

    {"det_su", GGenreSovietEraDetectives},

    {"sf_space", GGenreSpace},

    {"Космическая фантастика", GGenreSpaceFiction},
    {"kosmicheskaya",          GGenreSpaceFiction},

    {"sf_space_opera", GGenreSpaceOpera},

    {"religion_spirituality", GGenreSpirituality},

    {"home_sport", GGenreSports},
    {"sport",      GGenreSports},

    {"sf_stimpank", GGenreSteampunk},
    {"steampunk",   GGenreSteampunk},

    {"sci_crib", GGenreStudyGuides},

    {"альтернативная энергетика", GGenreTechnical},
    {"sci_metal",                 GGenreTechnical},
    {"sci_tech",                  GGenreTechnical},
    {"tech_all",                  GGenreTechnical},

    {"thriller_techno", GGenreTechnoThriller},

    {"teens_social", GGenreTeenFiction},

    {"dlya_podrostkov",  GGenreTeenLiterature},
    {"Juvenile Fiction", GGenreTeenLiterature},
    {"prose_teen",       GGenreTeenLiterature},
    {"teens_literature", GGenreTeenLiterature},

    {"teens_sf", GGenreTeenScienceFiction},

    {"sci_textbook", GGenreTextbooks},

    {"theatre", GGenreTheatre},

    {"ostrosyuzhetnyye", GGenreThrillers},
    {"Suspense",         GGenreThrillers},
    {"Suspense Fiction", GGenreThrillers},
    {"thriller",         GGenreThrillers},
    {"Thriller",         GGenreThrillers},
    {"Thrillers",        GGenreThrillers},
    {"trillery",         GGenreThrillers},

    {"popadantsy_vo_vremeni", GGenreTimeTravelFantasy},

    {"child_sf_hronoopera", GGenreTimeTravelFiction},
    {"hronoopera",          GGenreTimeTravelFiction},

    {"tragedy", GGenreTragedy},

    {"sci_transport", GGenreTransportationTechnology},

    {"Europe",                           GGenreTravel},
    {"Europe - Great Britain - General", GGenreTravel},
    {"Great Britain",                    GGenreTravel},
    {"outdoors_travel",                  GGenreTravel},
    {"puteshestviyah",                   GGenreTravel},
    {"travel",                           GGenreTravel},
    {"travel_africa",                    GGenreTravel},
    {"travel_asia",                      GGenreTravel},
    {"travel_europe",                    GGenreTravel},
    {"travel_ex_ussr",                   GGenreTravel},
    {"travel_lat_am",                    GGenreTravel},
    {"travel_polar",                     GGenreTravel},
    {"travel_spec",                      GGenreTravel},

    {"adv_geo",                   GGenreTravelGeography},
    {"Expeditions & Discoveries", GGenreTravelGeography},

    {"Путеводители",            GGenreTravelGuides},
    {"geo_guide",               GGenreTravelGuides},
    {"geo_guides",              GGenreTravelGuides},
    {"travel_guidebook_series", GGenreTravelGuides},

    {"travel_notes", GGenreTravelNotes},

    {"unfinished", GGenreUnfinishedWorks},

    {"city_fantasy",    GGenreUrbanFantasy},
    {"sf_fantasy_city", GGenreUrbanFantasy},
    {"urban_fantasy",   GGenreUrbanFantasy},

    {"утопия", GGenreUtopianFiction},

    {"vaudeville", GGenreVaudeville},

    {"sci_veterinary", GGenreVeterinaryMedicine},

    {"visual_arts", GGenreVisualArts},

    {"visual_poetry", GGenreVisualPoetry},

    {"knigi_o_voyne",  GGenreWarLiterature},
    {"literature_war", GGenreWarLiterature},

    {"adv_western",        GGenreWestern},
    {"literature_western", GGenreWestern},

    {"literature_women", GGenreWomensFiction},

    {"palindromes", GGenreWordplay},

    {"art_world_culture",              GGenreWorldArtsCulture},
    {"zarubezhnaya_kultura_iskusstvo", GGenreWorldArtsCulture},

    {"literature_world", GGenreWorldLiterature},

    {"nonf_biography_writers", GGenreWritersBiographies},

    {"sf_writing", GGenreWriting},

    {"fantasy_wuxia", GGenreWuxia},
    {"wuxia",         GGenreWuxia},

    {"ya",          GGenreYoungAdult},
    {"young_adult", GGenreYoungAdult},

    {"sci_zoo",   GGenreZoology},
    {"zoologiya", GGenreZoology},

};
// clang-format on

std::string_view TrimAsciiWhitespace(std::string_view value) noexcept
{
    const auto ltrim = value.find_first_not_of(" \t\r\n\v\f");
    if (ltrim == std::string_view::npos)
        return {};

    const auto rtrim = value.find_last_not_of(" \t\r\n\v\f");
    return value.substr(ltrim, rtrim - ltrim + 1);
}

std::string_view TrimBoundaryNoise(std::string_view value) noexcept
{
    value = TrimAsciiWhitespace(value);

    while (!value.empty() && (value.front() == '/' || value.front() == '('))
    {
        value.remove_prefix(1);
        value = TrimAsciiWhitespace(value);
    }

    while (!value.empty() && value.back() == ')')
    {
        value.remove_suffix(1);
        value = TrimAsciiWhitespace(value);
    }

    return value;
}

std::size_t FindFirstAsciiWhitespace(const std::string_view value) noexcept
{
    return value.find_first_of(" \t\r\n\v\f");
}

std::string_view LookupGenreName(const std::string_view code) noexcept
{
    if (const auto it = GGenreNames.find(code); it != GGenreNames.end())
        return it->second;
    return {};
}

SGenreResolution KnownGenre(const std::string_view displayName) noexcept
{
    return SGenreResolution{displayName, true};
}

} // namespace

SGenreResolution CFb2GenreMapper::ResolveGenre(const std::string_view fb2Code) noexcept
{
    // Fast path: exact match
    if (const std::string_view displayName = LookupGenreName(fb2Code); !displayName.empty())
        return KnownGenre(displayName);

    // Normalization: trim leading/trailing ASCII whitespace
    const std::string_view trimmed = TrimAsciiWhitespace(fb2Code);
    if (trimmed.empty())
        return KnownGenre({}); // all-whitespace → treat as empty

    if (trimmed.size() != fb2Code.size())
    {
        if (const std::string_view displayName = LookupGenreName(trimmed); !displayName.empty())
            return KnownGenre(displayName);
    }

    // Normalization: a few real records carry a stray boundary punctuation mark from broken tokenization.
    const std::string_view boundaryTrimmed = TrimBoundaryNoise(trimmed);
    if (boundaryTrimmed != trimmed)
    {
        if (const std::string_view displayName = LookupGenreName(boundaryTrimmed); !displayName.empty())
            return KnownGenre(displayName);
    }

    const std::string_view normalizedCandidate = boundaryTrimmed.empty() ? trimmed : boundaryTrimmed;

    // Normalization: many source exports use hyphenated aliases for underscore-based FB2 codes.
    if (normalizedCandidate.find('-') != std::string_view::npos)
    {
        std::string normalized = std::string{normalizedCandidate};
        std::replace(normalized.begin(), normalized.end(), '-', '_');
        if (const std::string_view displayName = LookupGenreName(normalized); !displayName.empty())
            return KnownGenre(displayName);
    }

    // Normalization: mixed-language label "ascii_code CyrillicWord" — try prefix before first space.
    // Handles real-world values such as "sci_history История".
    if (const auto spacePos = FindFirstAsciiWhitespace(normalizedCandidate); spacePos != std::string_view::npos)
    {
        const std::string_view prefix = normalizedCandidate.substr(0, spacePos);
        if (!prefix.empty())
        {
            if (const std::string_view displayName = LookupGenreName(prefix); !displayName.empty())
                return KnownGenre(displayName);
        }
    }

    // Normalization: ASCII code with appended non-ASCII bytes (mojibake suffix).
    // Handles values such as "accountingВухучет" where the real code is "accounting".
    // Find first byte ≥ 0x80 (start of any multi-byte UTF-8 sequence).
    std::size_t nonAsciiPos = 0;
    while (nonAsciiPos < normalizedCandidate.size()
           && static_cast<unsigned char>(normalizedCandidate[nonAsciiPos]) < 0x80)
        ++nonAsciiPos;
    if (nonAsciiPos > 0 && nonAsciiPos < normalizedCandidate.size())
    {
        const std::string_view asciiPrefix = TrimAsciiWhitespace(normalizedCandidate.substr(0, nonAsciiPos));
        if (!asciiPrefix.empty())
        {
            if (const std::string_view displayName = LookupGenreName(asciiPrefix); !displayName.empty())
                return KnownGenre(displayName);
        }

        if (const auto whitespacePos = FindFirstAsciiWhitespace(asciiPrefix); whitespacePos != std::string_view::npos)
        {
            const std::string_view prefixBeforeWhitespace = asciiPrefix.substr(0, whitespacePos);
            if (const std::string_view displayName = LookupGenreName(prefixBeforeWhitespace); !displayName.empty())
                return KnownGenre(displayName);
        }
    }

    if (normalizedCandidate != trimmed)
    {
        if (const std::string_view displayName = LookupGenreName(normalizedCandidate); !displayName.empty())
            return KnownGenre(displayName);
    }

    return SGenreResolution{fb2Code, false};
}

std::span<const std::string_view> CFb2GenreMapper::KnownGenreNames()
{
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        result.reserve(GGenreNames.size());
        for (const auto& [code, displayName] : GGenreNames)
        {
            static_cast<void>(code);
            result.push_back(displayName);
        }
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }();

    return names;
}

std::string_view CFb2GenreMapper::ResolveGenreName(const std::string_view fb2Code) noexcept
{
    return ResolveGenre(fb2Code).DisplayName;
}

} // namespace InpxWebReader::Fb2Parsing
