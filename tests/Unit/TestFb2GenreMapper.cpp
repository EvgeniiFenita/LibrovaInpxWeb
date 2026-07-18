#include <catch2/catch_test_macros.hpp>

#include "Parsing/Fb2GenreMapper.hpp"

#include <algorithm>
#include <string_view>

TEST_CASE("Fb2GenreMapper resolves known FB2 2.1 codes to display names", "[fb2-genre-mapper]")
{
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf")            == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_space")      == "Space");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("science_fiction") == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_horror")     == "Horror & Mystic");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("det_police")    == "Police Stories");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("det_hard")      == "Hard-Boiled");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("thriller")      == "Thrillers");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("detective")     == "Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adventure")     == "Adventure");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_western")   == "Western");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_geo")       == "Travel & Geography");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("children")          == "Children's");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("child_tale")        == "Fairy Tales");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("child_4")           == "Children's");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("child_characters")  == "Children's Characters");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("poetry")        == "Poetry");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("antique")       == "Antique Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("antique_myths") == "Myths, Legends & Epics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_math")      == "Mathematics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_history")   == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("science")       == "Science");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("comp_programming") == "Programming");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("computers")     == "Computers & Internet");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("ref_ref")       == "Reference");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("reference")     == "Reference");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonf_biography") == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction")    == "Non-Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion")      == "Religion");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("humor")         == "Humor");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("home_cooking")  == "Cooking");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("home_sex")      == "Erotica & Sex");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("home")          == "Home & Family");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_adv")          == "Adventure Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_history")      == "Historical Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_professionals")     == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biz_management")          == "Business Management");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_europe")           == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_guidebook_series") == "Travel Guides");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_short")        == "Short Stories");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_essay")        == "Essays");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_historical")        == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_rus_classsic") == "Russian Classic Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_asia")             == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_history_avant")        == "Alternative History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("people")                  == "People & Society");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_war")          == "War Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_british")      == "British Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_europe")          == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_poetry")       == "Poetry");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biz_economics")           == "Economics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_arts")              == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_world")           == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_cyber_punk")           == "Cyberpunk");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("thriller_police")         == "Police Thrillers");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction_politics")     == "Politics & Society");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_leaders")           == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sport")                   == "Sports");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biz_accounting")          == "Accounting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_ex_ussr")          == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("science_biolog")          == "Biology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("romance_historical")      == "Historical Romance");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_spirituality")   == "Spirituality");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_occult")         == "Occult");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_east")           == "Eastern Religions");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_buddhism")       == "Buddhism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction_folklor")      == "Folklore");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction_edu")          == "Education");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("health_self_help")        == "Self-Help & Health");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_asia")            == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("family_pregnancy")        == "Pregnancy & Parenting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_middle_east")     == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_military_science")== "Military History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_usa")             == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("horror_occult")           == "Occult Horror");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("family_parenting")        == "Parenting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_critic")       == "Literary Criticism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_drama")        == "Drama");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("family_health")           == "Family Health");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("chris_pravoslavie")       == "Orthodox Christianity");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("chris_edu")               == "Christian Education");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("child_9")                 == "Children's");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("family")                  == "Family");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("health_psy")              == "Health Psychology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("teens_sf")                == "Teen Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_fairy")        == "Fairy Tales");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_arts")              == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("outdoors_fauna")          == "Nature & Wildlife");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("love_sf")                 == "Romance Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("popular_business")        == "Popular Business");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("marketing")               == "Marketing");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("architecture")            == "Architecture");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_philology")           == "Philology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_writing")              == "Writing");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_economy")             == "Economics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("economics")               == "Economics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("management")              == "Management");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_medicine_alternative")== "Alternative Medicine");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("health_sex")              == "Erotica & Sex");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_postapocalyptic")      == "Post-Apocalyptic");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("SF")                      == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("публицистика")            == "Publicism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Детская литература")      == "Children's");
}

TEST_CASE("Fb2GenreMapper resolves lib.rus.ec community codes added from production scans", "[fb2-genre-mapper]")
{
    // High-frequency unmapped codes from real lib.rus.ec scan runs
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("network_literature")    == "Network Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("fanfiction")            == "Fanfiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_budda")        == "Buddhism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("rel_boddizm")           == "Buddhism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("histor_military")       == "Military History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("professional_law")      == "Jurisprudence");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("popadanec")             == "Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("popadancy")             == "Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction_law")        == "Jurisprudence");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("foreign_publicism")     == "Publicism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_popular")           == "Popular Science");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_litrpg")             == "LitRPG");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_stimpank")           == "Steampunk");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("psy_theraphy")          == "Psychology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("hronoopera")            == "Time-Travel Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("fantasy_dark")          == "Dark Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("city_fantasy")          == "Urban Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("folk_tale")             == "Folk Tales");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_history_avant")     == "Alternative History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("prose_rus_classics")    == "Russian Classic Prose");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("prose_su_classic")      == "Soviet Classic Prose");

    // Cyrillic free-form community tags
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("История")               == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Биографии и мемуары")   == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Боевая фантастика")     == "Action Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Фэнтези")               == "Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Фентези")               == "Fantasy");
    // Multi-word Cyrillic tag — must not be shadowed by the shorter "Фэнтези" key
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Фэнтези Юмор")         == "Fantasy & Humor");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("языкознание")           == "Linguistics");
}

TEST_CASE("Fb2GenreMapper resolves high-frequency scan-log codes (5+ occurrences)", "[fb2-genre-mapper]")
{
    // Prose & narrative
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("story")             == "Short Stories");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("great_story")       == "Novella");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("roman")             == "Novel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("essay")             == "Essays");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("unfinished")        == "Unfinished Works");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("prose_abs")         == "Absurdist Prose");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("prose_sentimental") == "Sentimental Prose");

    // Poetry, drama & theatre
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("drama")             == "Drama");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("comedy")            == "Comedy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("tragedy")           == "Tragedy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("theatre")           == "Theatre");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("screenplays")       == "Screenplays");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("lyrics")            == "Song Lyrics & Poetry");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("song_poetry")       == "Song Poetry");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("poetry_modern")     == "Modern Poetry");

    // SF & fantasy variants
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_all")            == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_space_opera")    == "Space Opera");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf_fantasy_irony")  == "Humorous Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci-fi")            == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("fantasy")           == "Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("dragon_fantasy")    == "Dragon Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("gothic_novel")      == "Gothic Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("litrpg")            == "LitRPG");

    // Detective & thriller variants
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("det_cozy")          == "Cozy Mysteries");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("det_su")            == "Soviet-Era Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("det_all")           == "Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("foreign_detective") == "Foreign Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("thriller_legal")    == "Legal Thrillers");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("thriller_medical")  == "Medical Thrillers");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("thriller_techno")   == "Techno-Thriller");

    // Romance & adventure
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("love_hard")         == "Erotic Romance");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("love_all")          == "Romance");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_all")           == "Adventure");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_modern")        == "Modern Adventure");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("adv_story")         == "Adventure Stories");

    // Arts
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("visual_arts")       == "Visual Arts");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("comics")            == "Comics & Graphic Novels");

    // Science & education
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_pedagogy")      == "Pedagogy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_cosmos")        == "Astronomy & Cosmology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_textbook")      == "Textbooks");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_ecology")       == "Ecology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_zoo")           == "Zoology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_botany")        == "Botany");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_radio")         == "Radio & Electronics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("tbg_higher")        == "Higher Education");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("tbg_secondary")     == "Secondary Education");

    // Travel
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("geo_guides")        == "Travel Guides");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_notes")      == "Travel Notes");

    // Humor
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("humor_satire")      == "Satire & Humor");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("humor_all")         == "Humor");

    // Young Adult
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("ya")                == "Young Adult");

    // Literature
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_20")     == "Literature");

    // Military
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("military")          == "Military");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("military_arts")     == "Martial Arts");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("military_weapon")   == "Military Technology");

    // Religion
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_islam")       == "Islam");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_catholicism") == "Catholicism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_hinduism")    == "Hinduism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_protestantism")== "Protestantism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_all")         == "Religion");

    // Non-Fiction umbrella
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonf_all")          == "Non-Fiction");

    // Business
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("accounting")        == "Accounting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("small_business")    == "Small Business");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("job_hunting")       == "Career & Job Search");

    // Misc
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("astrology")         == "Astrology");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sagas")             == "Sagas");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("dissident")         == "Dissident Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("folklore")          == "Folklore");

    // Fanfiction community tags
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("POV")               == "Fanfiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("AU")                == "Fanfiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("PWP")               == "Fanfiction");

    // Cyrillic high-frequency tags
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Попаданцы")         == "Isekai / Portal Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Юмор")              == "Humor");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Мифические существа")== "Mythical Creatures");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Мистика")           == "Mystic & Supernatural");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Романтика")         == "Romance");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Фантастика")        == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Стёб")              == "Parody & Satire");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Первый раз")        == "Fanfiction");
}

TEST_CASE("Fb2GenreMapper returns the raw code unchanged for unrecognized input", "[fb2-genre-mapper]")
{
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("custom_genre")    == "custom_genre");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("")                == "");
    // Pure garbage values from real scans: numeric, path-like, too short
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("999999")          == "999999");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("fictionbook.cs")  == "fictionbook.cs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("st")              == "st");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("N/K")             == "N/K");
    // All-whitespace: treated as empty, never creates a blank facet in the UI
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("   ")             == "");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("\t\n")            == "");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName(" \t\r\n ")        == "");
}

TEST_CASE("Fb2GenreMapper resolves high-frequency codes from 2026-04 production scan run", "[fb2-genre-mapper]")
{
    // Cyrillic religion aliases (#150: 48 + 4 occurrences)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Православие")     == "Orthodox Christianity");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Христианство")    == "Christianity");

    // Business & economics (#150: 21 + 6 + 4 occurrences)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("paper_work")      == "Office & Administration");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("global_economy")  == "Economics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("economics_ref")   == "Economics");

    // Property (#150: 6 occurrences)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("real_estate")     == "Real Estate");

    // Religion (#150: 5 occurrences)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion_judaism")== "Judaism");
}

TEST_CASE("Fb2GenreMapper resolves high-frequency codes from 2026-05 INPX scan", "[fb2-genre-mapper]")
{
    const auto requireGenre = [](const std::string_view code, const std::string_view expected)
    {
        CAPTURE(code);
        REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName(code) == expected);
    };

    requireGenre("fantasy_action", "Action Fantasy");
    requireGenre("samizdat", "Samizdat");
    requireGenre("beginning_authors", "New Authors");
    requireGenre("unrecognised", "Other");
    requireGenre("sci_psychology_popular", "Popular Psychology");
    requireGenre("music_dancing", "Dance");
    requireGenre("urban_fantasy", "Urban Fantasy");
    requireGenre("sociology_book", "Sociology");
    requireGenre("foreign_dramaturgy", "Foreign Drama");
    requireGenre("sketch", "Sketches");
    requireGenre("geography_book", "Geography");
    requireGenre("foreign_poetry", "Foreign Poetry");
    requireGenre("essays", "Essays");
    requireGenre("dark_fantasy", "Dark Fantasy");
    requireGenre("auto_regulations", "Automotive Regulations");
    requireGenre("psy_alassic", "Psychology");
    requireGenre("foreign_humor", "Foreign Humor");
    requireGenre("foreign_comp", "Foreign Computers");
    requireGenre("popadantsy_vo_vremeni", "Time-Travel Fantasy");
    requireGenre("asian_fantasy", "Asian Fantasy");
    requireGenre("home_collecting", "Collecting");
    requireGenre("popadantsy_v_magicheskie_miry", "Portal Fantasy");
    requireGenre("child_tale_russian_writers", "Russian Fairy Tales");
    requireGenre("foreign_desc", "Foreign Essays");
    requireGenre("slash", "Fanfiction");
    requireGenre("back_to_ussr", "Alternative History");
    requireGenre("novel", "Novel");
    requireGenre("epic_poetry", "Epic Poetry");
    requireGenre("in_verse", "Poetry");
    requireGenre("religion_paganism", "Paganism");
    requireGenre("Policier", "Police Stories");
    requireGenre("extravaganza", "Extravaganza");
    requireGenre("fantasy_heroic", "Heroic Fantasy");
    requireGenre("Maigret", "Detectives");
    requireGenre("Учебные заведения", "School Stories");
    requireGenre("foreign_action", "Foreign Action Fiction");
    requireGenre("popadantsy", "Portal Fantasy");
    requireGenre("Adventure", "Adventure");
    requireGenre("poetry_rus_classical", "Russian Classical Poetry");
    requireGenre("Action", "Action Fiction");
    requireGenre("epic", "Epic");
    requireGenre("fantasy_epic", "Epic Fantasy");
    requireGenre("Экшн (action)", "Action Fiction");
    requireGenre("scenarios", "Scenarios");
    requireGenre("Dystopian", "Dystopian");
    requireGenre("magic_school", "Magic School");
    requireGenre("child_det_children_detectives", "Children's Detectives");
    requireGenre("child_tale_foreign_writers", "Foreign Fairy Tales");
    requireGenre("Humor", "Humor");
    requireGenre("child_adv_animal", "Animal Adventure");
    requireGenre("child_sf_fantasy", "Children's Fantasy");
    requireGenre("child_det_animal_detectives", "Animal Detectives");
    requireGenre("child_folklore", "Children's Folklore");
    requireGenre("historical-fiction", "Historical Fiction");
    requireGenre("vers_libre", "Free Verse");
    requireGenre("Ангст", "Fanfiction");
    requireGenre("child_prose_history", "Children's Historical Prose");
    requireGenre("fan_translation", "Fan Translation");
    requireGenre("realrpg", "LitRPG");
    requireGenre("Romance", "Romance");
    requireGenre("sci_biochem", "Biochemistry");
    requireGenre("vaudeville", "Vaudeville");
    requireGenre("popadantsy-v-magicheskie-miry", "Portal Fantasy");
    requireGenre("postapocalyptic", "Post-Apocalyptic");
    requireGenre("Drama", "Drama");
    requireGenre("nonf_biography_writers", "Writers' Biographies");
    requireGenre("poetry_classical", "Classical Poetry");
    requireGenre("zarubezhnaya_klassika", "Foreign Classics");
    requireGenre("20_vek", "20th Century Literature");
    requireGenre("Драма", "Drama");
    requireGenre("Дружба", "Friendship");
    requireGenre("Повседневность", "Slice of Life");
    requireGenre("Философия", "Philosophy");
    requireGenre("boyar-anime", "Anime-style Fiction");
    requireGenre("child_tale_rus", "Russian Fairy Tales");
    requireGenre("dorama", "Drama");
    requireGenre("everyday_fantasy", "Everyday Fantasy");
    requireGenre("fantasy_det", "Fantasy Detective");
    requireGenre("geo_guide", "Travel Guides");
    requireGenre("poetry_rus_modern", "Modern Russian Poetry");
    requireGenre("proverbs", "Proverbs");
    requireGenre("tbg_computers", "Computer Education");
    requireGenre("child_classical", "Children's Classics");
    requireGenre("child_prose_humor", "Children's Humor");
    requireGenre("experimental_poetry", "Experimental Poetry");
    requireGenre("fable", "Fables");
    requireGenre("fantasy-erotika", "Erotic Fantasy");
    requireGenre("General", "General");
    requireGenre("poem", "Poetry");
    requireGenre("poetry_east", "Eastern Poetry");
    requireGenre("tbg_school", "School Education");
    requireGenre("ЛитРПГ", "LitRPG");
    requireGenre("Angst", "Fanfiction");
    requireGenre("dramaturgy_all", "Dramaturgy");
    requireGenre("limerick", "Limericks");
    requireGenre("military_all", "Military");
    requireGenre("nonf_biography_historical", "Biography & Memoirs");
    requireGenre("sci_biophys", "Biophysics");
    requireGenre("sci_crib", "Study Guides");
    requireGenre("skazki", "Fairy Tales");
    requireGenre("sovremennye", "Contemporary Fiction");
    requireGenre("collection", "Collection");
    requireGenre("comp_all", "Computers & Internet");
    requireGenre("dystopia", "Dystopian");
    requireGenre("knigi_sovremennaya_proza_zarubezhnaya", "Contemporary Foreign Fiction");
    requireGenre("poetry_for_classical", "Classical Poetry");
    requireGenre("ref_all", "Reference");
    requireGenre("sci_veterinary", "Veterinary Medicine");
    requireGenre("steampunk", "Steampunk");
    requireGenre("zarubezhnyye", "Foreign Literature");
}

TEST_CASE("Fb2GenreMapper resolves long-tail raw tags from the 2026-05 large INPX scan", "[fb2-genre-mapper]")
{
    struct SExpectedGenre final
    {
        std::string_view Raw;
        std::string_view Expected;
    };

    const SExpectedGenre genres[] = {
        {"action",                                                     "Action Fiction"},
        {"19_vek",                                                     "19th Century Literature"},
        {"about_musicians",                                            "Music"},
        {"child_det_other",                                            "Children's Detectives"},
        {"nonf_biography_celebrities",                                 "Biography & Memoirs"},
        {"prose_all",                                                  "Prose"},
        {"prose_neformatny",                                           "Experimental Prose"},
        {"sexual_perversion",                                          "Erotica & Sex"},
        {"sf_humory",                                                  "Humorous SF"},
        {"sf_litRPG",                                                  "LitRPG"},
        {"slavic_fantasy",                                             "Slavic Fantasy"},
        {"sovremennyye",                                               "Contemporary Fiction"},
        {"this is science",                                            "Science"},
        {"trade",                                                      "Business"},
        {"child_sf_horror",                                            "Children's Science Fiction"},
        {"compilation",                                                "Anthology"},
        {"det_rus",                                                    "Detectives"},
        {"detective-science-fiction",                                  "Detective"},
        {"detskie_knigi_zarubezhnye",                                  "Foreign Children's Books"},
        {"epic-fantasy",                                               "Epic Fantasy"},
        {"folklore_all",                                               "Folklore"},
        {"foreign_novel",                                              "Foreign Prose"},
        {"heroic-fantasy",                                             "Heroic Fantasy"},
        {"historical-adventure",                                       "Adventure"},
        {"knigi_detektivy_zarubezhnye",                                "Foreign Detectives"},
        {"knigi_fentezi_zarubezhnye",                                  "Foreign Fantasy"},
        {"nemetckij_yazyk",                                            "Foreign Languages"},
        {"nonf_biography_military_figures",                            "Biography & Memoirs"},
        {"palindromes",                                                "Wordplay"},
        {"palmistry",                                                  "Esoterics"},
        {"Parody",                                                     "Parody & Satire"},
        {"prose_epic",                                                 "Epic"},
        {"riddles",                                                    "Riddles"},
        {"sf_paleontological",                                         "Science Fiction"},
        {"wuxia",                                                      "Wuxia"},
        {"young_adult",                                                "Young Adult"},
        {"Даркфик",                                                    "Fanfiction"},
        {"Детектив",                                                   "Detectives"},
        {"Исторические эпохи",                                         "History"},
        {"Пародия",                                                    "Parody & Satire"},
        {"Современная Проза",                                          "Contemporary Prose"},
        {"Фанфик",                                                     "Fanfiction"},
        {"adult",                                                      "Adult Fiction"},
        {"adv_military",                                               "Military Adventure"},
        {"Biography & Autobiography",                                  "Biography & Memoirs"},
        {"boevaya_fantastika",                                         "Action SF"},
        {"computer_translation",                                       "Computers & Internet"},
        {"Crossover",                                                  "Fanfiction"},
        {"Darkfic",                                                    "Fanfiction"},
        {"erotic_fantasy",                                             "Erotic Fantasy"},
        {"fantasy_rus",                                                "Russian Fantasy"},
        {"foreign_story",                                              "Foreign Prose"},
        {"Hurt/comfort",                                               "Fanfiction"},
        {"ironical-fantasy",                                           "Humorous Fantasy"},
        {"istoricheskaya_literatura",                                  "Historical Fiction"},
        {"istorii-iz-zhizni",                                          "Slice of Life"},
        {"kinematograf_teatr",                                         "Cinema & Theatre"},
        {"klassicheskie",                                              "Classic Literature"},
        {"kosmicheskaya",                                              "Space Fiction"},
        {"kulturologiya",                                              "Cultural Studies"},
        {"legkaya_proza",                                              "Light Fiction"},
        {"lubov",                                                      "Romance"},
        {"nauchnaya",                                                  "Science"},
        {"newspapers",                                                 "Periodicals"},
        {"nonfiction_social_sci",                                      "Social Studies"},
        {"ostrosyuzhetnyye",                                           "Thrillers"},
        {"paranormal",                                                 "Paranormal Fiction"},
        {"poetry_all",                                                 "Poetry"},
        {"poetry_for_modern",                                          "Modern Poetry"},
        {"priklucheniya",                                              "Adventure"},
        {"puteshestviyah",                                             "Travel"},
        {"russkaya_literatura",                                        "Russian Literature"},
        {"sf_love",                                                    "Romance Science Fiction"},
        {"Songfic",                                                    "Fanfiction"},
        {"trillery",                                                   "Thrillers"},
        {"zarubezhnaya",                                               "Foreign Literature"},
        {"Боевое фэнтези",                                             "Action Fantasy"},
        {"Математика",                                                 "Mathematics"},
        {"Постапокалиптика",                                           "Post-Apocalyptic"},
        {"Религия",                                                    "Religion"},
        {"Самосовершенствование",                                      "Self-Improvement"},
        {"Флафф",                                                      "Fanfiction"},
        {"Экономика",                                                  "Economics"},
        {"Эксперимент",                                                "Experimental Fiction"},
        {"поэзия",                                                     "Poetry"},
        {"психология",                                                 "Psychology"},
        {"(network_literature",                                        "Network Literature"},
        {"/sf_action",                                                 "Action SF"},
        {"105",                                                        "Other"},
        {"1979",                                                       "Other"},
        {"19th century",                                               "History"},
        {"?",                                                          "Other"},
        {"accounting\tР‘СѓС…СѓС‡РµС‚",                                 "Accounting"},
        {"American First Novelists",                                   "American Literature"},
        {"American Mystery & Suspense Fiction",                        "Mystery Thriller"},
        {"antichnaya",                                                 "Antique Literature"},
        {"Assassins",                                                  "Crime"},
        {"auto_business",                                              "Business"},
        {"Autobiografia",                                              "Biography & Memoirs"},
        {"biografii_i_memuary",                                        "Biography & Memoirs"},
        {"biologiya_i_himiya",                                         "Science"},
        {"Black Humor (Literature)",                                   "Satire & Humor"},
        {"Boys & Men",                                                 "Fiction"},
        {"Case studies",                                               "Non-Fiction"},
        {"child_all",                                                  "Children's"},
        {"child_det_children",                                         "Children's Detectives"},
        {"child_nature",                                               "Nature & Animals"},
        {"child_prose_romantic",                                       "Children's Prose"},
        {"child_sf_hronoopera",                                        "Time-Travel Fiction"},
        {"child_sf_space",                                             "Children's Science Fiction"},
        {"ci_history",                                                 "History"},
        {"det_lady",                                                   "Detectives"},
        {"detakaya_proza",                                             "Children's Prose"},
        {"detective_fentezi",                                          "Fantasy Detective"},
        {"Detectives - England - London",                              "Detectives"},
        {"detektive",                                                  "Detectives"},
        {"detskaya_pazvivayushaya_literatura",                         "Educational"},
        {"detskie_detektivy",                                          "Children's Detectives"},
        {"dlya_podrostkov",                                            "Teen Literature"},
        {"dokumentalnaya_literatura",                                  "Non-Fiction"},
        {"domashnie_zhivotnye",                                        "Pets"},
        {"drakony",                                                    "Dragon Fantasy"},
        {"drevnevostochnaya",                                          "Old Eastern"},
        {"England",                                                    "Other"},
        {"Europe",                                                     "Travel"},
        {"Europe - Great Britain - General",                           "Travel"},
        {"evoljuciya_i_antropologiya",                                 "Anthropology"},
        {"Expeditions & Discoveries",                                  "Travel & Geography"},
        {"f",                                                          "Other"},
        {"f_fantasy",                                                  "Fantasy"},
        {"fairy-tale",                                                 "Fairy Tales"},
        {"fanficion",                                                  "Fanfiction"},
        {"fantasy_wuxia",                                              "Wuxia"},
        {"femslash",                                                   "Fanfiction"},
        {"Fiction - Espionage",                                        "Espionage"},
        {"Fiction - General",                                          "Fiction"},
        {"folklor",                                                    "Folklore"},
        {"geroicheskoe_fentezi",                                       "Heroic Fantasy"},
        {"Great Britain",                                              "Travel"},
        {"greate_story",                                               "Novella"},
        {"hand-book",                                                  "Reference"},
        {"History: World",                                             "History"},
        {"home_all",                                                   "Home & Family"},
        {"horror_usa",                                                 "Horror"},
        {"interview",                                                  "Non-Fiction"},
        {"ironicheskie",                                               "Ironical Detectives"},
        {"istoricheskaya",                                             "Historical Fiction"},
        {"Juvenile Fiction",                                           "Teen Literature"},
        {"klassicheskaya_literatura_russkaya",                         "Russian Classic Literature"},
        {"klassika-fantastiki",                                        "Science Fiction"},
        {"klassika_priklyuchenij",                                     "Adventure Literature"},
        {"knigi_detektivy_istoricheskie",                              "Historical Detectives"},
        {"knigi_o_voyne",                                              "War Literature"},
        {"kosmos_i_vselennaya",                                        "Astronomy & Cosmology"},
        {"kriminologiya",                                              "Criminology"},
        {"Lang:fr",                                                    "Other"},
        {"Law Enforcement",                                            "Police Stories"},
        {"London",                                                     "Other"},
        {"long_story",                                                 "Novel"},
        {"Medical",                                                    "Medicine"},
        {"mify_legendy_epos",                                          "Myths, Legends & Epics"},
        {"mistika",                                                    "Mystic & Supernatural"},
        {"Murder",                                                     "Crime"},
        {"Murder - England - Wiltshire - History - 19th century",      "Crime Non-Fiction"},
        {"Murder - General",                                           "Crime"},
        {"muzei_kollektsii",                                           "Museums & Collections"},
        {"Mysteries & Thrillers",                                      "Mystery Thriller"},
        {"Mystery & Detective",                                        "Detectives"},
        {"Mystery & Detective Fiction",                                "Detectives"},
        {"nauchno_populyarnaya_literatura",                            "Popular Science"},
        {"nejrobiologiya",                                             "Neurobiology"},
        {"nonfiction_social_work",                                     "Social Work"},
        {"nonfiction_sociology",                                       "Sociology"},
        {"obschaya_psihologiya",                                       "Psychology"},
        {"oratorskoye_iskusstvo",                                      "Public Speaking"},
        {"Organized Crime",                                            "Crime"},
        {"other_all",                                                  "Other"},
        {"Physicians",                                                 "Medicine"},
        {"Physicists",                                                 "Physics"},
        {"poeziya",                                                    "Poetry"},
        {"Political",                                                  "Politics"},
        {"populyarnaya_istoriya",                                      "History"},
        {"priklucheniya_knigi",                                        "Adventure"},
        {"prose_teen",                                                 "Teen Literature"},
        {"ref_books",                                                  "Reference"},
        {"Relativity",                                                 "Physics"},
        {"Relativity (Physics)",                                       "Physics"},
        {"russian",                                                    "Russian Literature"},
        {"russkaya_poeziya",                                           "Russian Poetry"},
        {"saga",                                                       "Sagas"},
        {"samorazvitiye_lichnostny_rost",                              "Self-Improvement"},
        {"sci_abstract",                                               "Science"},
        {"sci_archeology",                                             "Archaeology"},
        {"sci_history_20",                                             "History"},
        {"sci_history_21",                                             "History"},
        {"sci_metal",                                                  "Technical"},
        {"sci_orgchem",                                                "Chemistry"},
        {"sci_physchem",                                               "Chemistry"},
        {"sci_social_studies)",                                        "Social Studies"},
        {"Science & Technology",                                       "Science"},
        {"sf_litrp",                                                   "LitRPG"},
        {"sf_rus",                                                     "Russian Science Fiction"},
        {"snetwork_literature",                                        "Network Literature"},
        {"su_publication",                                             "Soviet Literature"},
        {"Suspense",                                                   "Thrillers"},
        {"Suspense Fiction",                                           "Thrillers"},
        {"tech_all",                                                   "Technical"},
        {"Thriller",                                                   "Thrillers"},
        {"True Crime",                                                 "Crime Non-Fiction"},
        {"umori_i_satira",                                             "Satire & Humor"},
        {"umoristicheskaya_proza",                                     "Humorous Prose"},
        {"Unified Field Theories",                                     "Physics"},
        {"visual_poetry",                                              "Visual Poetry"},
        {"voennoe_delo_specsluzhby",                                   "Military"},
        {"War & Military",                                             "Military"},
        {"Whicher; Jonathan",                                          "Other"},
        {"Wiltshire",                                                  "Other"},
        {"zarubezhnaya_kultura_iskusstvo",                             "World Arts & Culture"},
        {"zarubezhnaya_obrazovatelnaya_literatura",                    "Education"},
        {"zarubezhnaya_prikladnaya_literatura",                        "Non-Fiction"},
        {"zarubezhnaya_starinnaya",                                    "Foreign Classics"},
        {"zarubezhnye",                                                "Foreign Literature"},
        {"zarubezhnyy_umor",                                           "Foreign Humor"},
        {"zoologiya",                                                  "Zoology"},
        {"Історія",                                                    "History"},
        {"Альтернативная медицина",                                    "Alternative Medicine"},
        {"Антиутопия",                                                 "Dystopian"},
        {"Джен",                                                       "Fanfiction"},
        {"Журналы",                                                    "Periodicals"},
        {"Исторические любовные романы",                               "Historical Romance"},
        {"Казки",                                                      "Fairy Tales"},
        {"Классическая проза",                                         "Classic Prose"},
        {"Комедия",                                                    "Comedy"},
        {"Любовное фэнтези",                                           "Fantasy Romance"},
        {"Любовь/Ненависть",                                           "Romance"},
        {"НФ",                                                         "Science Fiction"},
        {"Науково- практичний коментар",                               "Jurisprudence"},
        {"Недвижимость",                                               "Real Estate"},
        {"Приключенческое фэнтези",                                    "Adventure Fantasy"},
        {"Пропущенная сцена",                                          "Fanfiction"},
        {"Публицистика: прочее",                                       "Publicism"},
        {"Путеводители",                                               "Travel Guides"},
        {"РеалРПГ",                                                    "LitRPG"},
        {"Романы",                                                     "Novel"},
        {"Фентъзи",                                                    "Fantasy"},
        {"Эротическое фэнтези",                                        "Erotic Fantasy"},
        {"альтернативная энергетика",                                  "Technical"},
        {"анархизм",                                                   "Politics & Society"},
        {"анархия",                                                    "Politics & Society"},
        {"геополитика",                                                "Politics & Society"},
        {"исторический детектив",                                      "Historical Detectives"},
        {"история",                                                    "History"},
        {"миры eve",                                                   "Science Fiction"},
        {"научная фантастика",                                         "Science Fiction"},
        {"поезія",                                                     "Poetry"},
        {"попаданец",                                                  "Isekai / Portal Fantasy"},
        {"романтика",                                                  "Romance"},
        {"сельское хозяйство",                                         "Agriculture"},
        {"современная фантастика",                                     "Science Fiction"},
        {"темная фантастика",                                          "Dark Fantasy"},
        {"устойчивое развитие",                                        "Ecology"},
        {"утопия",                                                     "Utopian Fiction"},
        {"фантастические существа",                                    "Mythical Creatures"},
        {"фантастический мир",                                         "Fantasy"},
        {"фэнтези",                                                    "Fantasy"},
        {"экология",                                                   "Ecology"},
        {"экономика дара",                                             "Economics"},
        {"ю",                                                          "Other"},
        {"юмористическая проза",                                       "Humorous Prose"},
        {"юмористическая фантастика",                                  "Humorous SF"},
    };

    for (const SExpectedGenre& genre : genres)
    {
        CAPTURE(genre.Raw);
        REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName(genre.Raw) == genre.Expected);
    }
}

TEST_CASE("Fb2GenreMapper normalizes mixed-language and mojibake codes", "[fb2-genre-mapper]")
{
    // Mixed-language: "known_code CyrillicWord" — resolves via ASCII prefix before the space
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_history История")  == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf История")           == "Science Fiction");

    // Mojibake: ASCII code with appended non-ASCII bytes — resolves via ASCII prefix
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("accounting\xD0\x91\xD1\x83\xD1\x85") == "Accounting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("accounting\tР‘СѓС…СѓС‡РµС‚") == "Accounting");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("religion\xD1\x8f")     == "Religion");

    // Whitespace trimming
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("  sf  ")               == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("\t humor \n")           == "Humor");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sf-action")             == "Action SF");

    // Stray punctuation from malformed source tokenization is ignored only at token boundaries.
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("/sf_action")            == "Action SF");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("(network_literature")   == "Network Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("sci_social_studies)")   == "Social Studies");
}

TEST_CASE("Fb2GenreMapper consolidates regional sub-genres to single filter entries", "[fb2-genre-mapper]")
{
    // All biogr_* → "Biography & Memoirs" (#122)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_professionals") == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_historical")    == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_arts")          == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_leaders")       == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_travel")        == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biogr_sports")        == "Biography & Memoirs");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("biz_beogr")           == "Biography & Memoirs");

    // Regional travel variants → "Travel"; guidebooks stay distinct (#122)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_polar")        == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_europe")       == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_asia")         == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_ex_ussr")      == "Travel");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("travel_guidebook_series") == "Travel Guides");

    // Regional history variants → "History"; military history stays distinct (#122)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_russia")      == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_europe")      == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_world")       == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_asia")        == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_middle_east") == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_usa")         == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("history_military_science") == "Military History");

    // Century literature codes → "Literature" (#122)
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_18")       == "Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_19")       == "Literature");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("literature_20")       == "Literature");
}

TEST_CASE("Fb2GenreMapper resolves raw display-label aliases from the 2026-05 INPX scan", "[fb2-genre-mapper]")
{
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("dystopian")          == "Dystopian");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Fiction")            == "Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("fiction")            == "Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("History")            == "History");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Detective")          == "Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Fantasy")            == "Fantasy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("health")             == "Health");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Biography")          == "Biography");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("business")           == "Business");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Comedy")             == "Comedy");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("cyberpunk")          == "Cyberpunk");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("medicine")           == "Medicine");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Thrillers")          == "Thrillers");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Detectives")         == "Detectives");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Economics")          == "Economics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Espionage")          == "Espionage");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Literary Criticism") == "Literary Criticism");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("LitRpg")             == "LitRPG");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Physics")            == "Physics");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Prose")              == "Prose");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Science")            == "Science");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Science Fiction")    == "Science Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("nonfiction_spec_group") == "Non-Fiction");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("Современная проза")  == "Contemporary Prose");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("современная проза")  == "Contemporary Prose");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("фанфик")             == "Fanfiction");
}

TEST_CASE("Fb2GenreMapper resolution is case-sensitive", "[fb2-genre-mapper]")
{
    // Canonical FB2 codes are lowercase; arbitrary uppercase variants are not recognised codes.
    // Exceptions such as 'SF' and specific title-case free-text aliases are explicitly mapped.
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("SF_SPACE")   == "SF_SPACE");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("AdVeNtUrE")  == "AdVeNtUrE");
    REQUIRE(InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenreName("THRILLER")   == "THRILLER");
}

TEST_CASE("Fb2GenreMapper reports whether a raw-looking display name was actually mapped", "[fb2-genre-mapper]")
{
    const InpxWebReader::Fb2Parsing::SGenreResolution adventure =
        InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenre("Adventure");
    REQUIRE(adventure.DisplayName == "Adventure");
    REQUIRE(adventure.IsKnown);

    const InpxWebReader::Fb2Parsing::SGenreResolution unknown =
        InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenre("custom_genre");
    REQUIRE(unknown.DisplayName == "custom_genre");
    REQUIRE_FALSE(unknown.IsKnown);

    const InpxWebReader::Fb2Parsing::SGenreResolution whitespace =
        InpxWebReader::Fb2Parsing::CFb2GenreMapper::ResolveGenre(" \t\r\n ");
    REQUIRE(whitespace.DisplayName.empty());
    REQUIRE(whitespace.IsKnown);
}

TEST_CASE("Fb2GenreMapper exposes the complete sorted canonical display-name set", "[fb2-genre-mapper]")
{
    const auto names = InpxWebReader::Fb2Parsing::CFb2GenreMapper::KnownGenreNames();

    REQUIRE(names.size() == 395);
    REQUIRE(std::ranges::is_sorted(names));
    REQUIRE(std::ranges::adjacent_find(names) == names.end());
    REQUIRE(std::ranges::binary_search(names, std::string_view{"Adventure"}));
    REQUIRE(std::ranges::binary_search(names, std::string_view{"Science Fiction"}));
}
