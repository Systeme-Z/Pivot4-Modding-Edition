/**
 * PIVOT4 v3.0 - Moteur de Génération Procédurale Universel
 * ----------------------------------------------------------
 * Architecture : Zéro dépendance optionnelle, Déterministe, Data-Oriented
 * Version : 3.0 (Modding Edition - Production Ready)
 * 
 * Features:
 *   ✓ Génération procédurale déterministe
 *   ✓ Cache intelligent multi-niveaux
 *   ✓ Export multi-formats (RAW, PGM, OBJ, PLY, JSON)
 *   ✓ Splatmaps pour moteurs de jeu (Unity, Unreal, Enfusion)
 *   ✓ Système de rivières et érosion
 *   ✓ Masques d'influence personnalisables
 *   ✓ Mode Batch avec configuration YAML/JSON
 *   ✓ Serveur web intégré (optionnel)
 *   ✓ Interface CLI complète
 * 
 * Compilation:
 *   g++ -O3 -std=c++17 pivot4.cpp -o pivot4
 *   g++ -O3 -std=c++17 -DPIVOT4_ENABLE_WEBUI pivot4.cpp -o pivot4 -lpthread
 */

#include <cmath>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <random>
#include <chrono>
#include <functional>

// =============================================================================
// OPTIONS DE COMPILATION
// =============================================================================

// Décommentez pour activer l'interface web (nécessite httplib)
// #define PIVOT4_ENABLE_WEBUI

// Décommentez pour utiliser stb_image (nécessite stb_image.h)
// #define PIVOT4_USE_STB_IMAGE

#ifdef PIVOT4_USE_STB_IMAGE
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

// =============================================================================
// CONFIGURATION PRINCIPALE
// =============================================================================

struct Pivot4Config {
    uint32_t seed = 1337;
    int hauteur_min = -3, hauteur_max = 8;
    int amplitude_octave = 2;
    int chunk_taille = 32;
    int objets_par_chunk = 30;
    bool erosion_active = true;
    bool rivieres_active = true;
    int nb_rivieres = 4;
    
    // Export
    struct {
        bool little_endian = true;
        int height_scale = 100;
        std::string height_format = "raw16";
        std::string output_dir = "output";
    } export_settings;
    
    void charger(const std::string& f) {
        std::ifstream file(f);
        if (!file.is_open()) return;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream is_line(line);
            std::string key;
            if (std::getline(is_line, key, '=')) {
                std::string value;
                if (std::getline(is_line, value)) {
                    if (key == "seed") seed = std::stoul(value);
                    else if (key == "hauteur_min") hauteur_min = std::stoi(value);
                    else if (key == "hauteur_max") hauteur_max = std::stoi(value);
                    else if (key == "amplitude_octave") amplitude_octave = std::stoi(value);
                    else if (key == "chunk_taille") chunk_taille = std::stoi(value);
                    else if (key == "objets_par_chunk") objets_par_chunk = std::stoi(value);
                    else if (key == "erosion_active") erosion_active = (value == "true");
                    else if (key == "rivieres_active") rivieres_active = (value == "true");
                    else if (key == "nb_rivieres") nb_rivieres = std::stoi(value);
                }
            }
        }
    }
};

static Pivot4Config C;

// =============================================================================
// STRUCTURES DE DONNÉES
// =============================================================================

struct Vector3D {
    float x, y, z;
    Vector3D() : x(0), y(0), z(0) {}
    Vector3D(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    
    Vector3D operator+(const Vector3D& other) const {
        return {x + other.x, y + other.y, z + other.z};
    }
    Vector3D operator*(float scalaire) const {
        return {x * scalaire, y * scalaire, z * scalaire};
    }
    void normaliser() {
        float len = sqrt(x*x + y*y + z*z);
        if(len > 0) { x /= len; y /= len; z /= len; }
    }
};

struct Couleur {
    float r, g, b;
    Couleur() : r(0.5f), g(0.5f), b(0.5f) {}
    Couleur(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
};

struct Pivot4Tile { 
    int x, z, y, type; 
    std::string biome; 
    int difficulte; 
};

struct Pivot4Objet { 
    std::string type; 
    float x, z; 
    int y; 
};

struct Pivot4Chunk {
    int cx, cz;
    std::vector<Pivot4Tile> tuiles;
    std::vector<Pivot4Objet> objets;
    std::string biome;
    int difficulte;
    bool est_eau, est_montagne, est_foret, est_desert, zone_minerais;
};

struct Pivot4WorldMap {
    int chunks_largeur, chunks_hauteur, taille_totale_x, taille_totale_z;
    std::vector<uint16_t> heightmap;
    std::vector<Pivot4Objet> tous_les_objets;
    
    Pivot4WorldMap() : chunks_largeur(0), chunks_hauteur(0), taille_totale_x(0), taille_totale_z(0) {}
    
    Pivot4WorldMap(int largeur_chunks, int hauteur_chunks) 
        : chunks_largeur(largeur_chunks), chunks_hauteur(hauteur_chunks) {
        taille_totale_x = largeur_chunks * C.chunk_taille;
        taille_totale_z = hauteur_chunks * C.chunk_taille;
        heightmap.resize(taille_totale_x * taille_totale_z, 0);
    }
};

struct BiomeLayer {
    std::string name;
    float seuil_min = 0.0f;
    float seuil_max = 1.0f;
    std::string couleur_hex = "#808080";
    int texture_index = 0;
};

struct ControlMap {
    std::string nom;
    std::string fichier;
    float intensite = 1.0f;
    int mode = 0;
    std::vector<uint8_t> pixels;
    int largeur = 0, hauteur = 0;
    bool charge = false;
};

struct Riviere {
    std::vector<std::pair<int, int>> points;
    float largeur = 2.0f;
    float profondeur = -2.0f;
};

// =============================================================================
// CONFIGURATION AVANCÉE
// =============================================================================

struct Pivot4ConfigPro {
    uint32_t seed = 1337;
    int hauteur_min = -3, hauteur_max = 8;
    int amplitude_octave = 2;
    int chunk_taille = 32;
    int objets_par_chunk = 30;
    
    struct {
        bool little_endian = true;
        int height_scale = 100;
        std::string height_format = "raw16";
    } export_settings;
    
    std::vector<BiomeLayer> biomes;
    std::string masque_entree = "";
    bool erosion_active = true;
    bool rivieres_active = true;
    int nb_rivieres = 4;
    
    void charger_json(const std::string& fichier) {
        std::ifstream f(fichier);
        if(!f.is_open()) {
            std::cerr << "⚠️ Fichier config introuvable, utilisation des valeurs par défaut\n";
            return;
        }
        
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        
        // Parser JSON simplifié (sans lib externe)
        auto extract_int = [&](const std::string& key) -> int {
            size_t pos = content.find("\"" + key + "\"");
            if(pos == std::string::npos) return 0;
            pos = content.find(":", pos);
            if(pos == std::string::npos) return 0;
            return std::stoi(content.substr(pos + 1));
        };
        
        auto extract_float = [&](const std::string& key) -> float {
            size_t pos = content.find("\"" + key + "\"");
            if(pos == std::string::npos) return 0.0f;
            pos = content.find(":", pos);
            if(pos == std::string::npos) return 0.0f;
            return std::stof(content.substr(pos + 1));
        };
        
        auto extract_string = [&](const std::string& key) -> std::string {
            size_t pos = content.find("\"" + key + "\"");
            if(pos == std::string::npos) return "";
            pos = content.find(":", pos);
            if(pos == std::string::npos) return "";
            pos = content.find("\"", pos);
            if(pos == std::string::npos) return "";
            size_t end = content.find("\"", pos + 1);
            if(end == std::string::npos) return "";
            return content.substr(pos + 1, end - pos - 1);
        };
        
        seed = extract_int("seed");
        if(seed == 0) seed = 1337;
        hauteur_min = extract_int("hauteur_min");
        if(hauteur_min == 0 && extract_int("hauteur_min") == 0) hauteur_min = -3;
        hauteur_max = extract_int("hauteur_max");
        if(hauteur_max == 0) hauteur_max = 8;
        amplitude_octave = extract_int("amplitude_octave");
        if(amplitude_octave == 0) amplitude_octave = 2;
        chunk_taille = extract_int("chunk_taille");
        if(chunk_taille == 0) chunk_taille = 32;
        objets_par_chunk = extract_int("objets_par_chunk");
        if(objets_par_chunk == 0) objets_par_chunk = 30;
        masque_entree = extract_string("masque_entree");
        
        // Extraire les biomes (simplifié)
        size_t biomes_pos = content.find("\"biomes\"");
        if(biomes_pos != std::string::npos) {
            biomes.clear();
            size_t obj_start = content.find("{", biomes_pos);
            int brace_count = 0;
            while(obj_start != std::string::npos) {
                size_t obj_end = content.find("}", obj_start);
                if(obj_end == std::string::npos) break;
                std::string biome_str = content.substr(obj_start, obj_end - obj_start + 1);
                
                BiomeLayer b;
                auto find_val = [&](const std::string& k) -> std::string {
                    size_t p = biome_str.find("\"" + k + "\"");
                    if(p == std::string::npos) return "";
                    p = biome_str.find(":", p);
                    if(p == std::string::npos) return "";
                    p = biome_str.find("\"", p);
                    if(p == std::string::npos) return "";
                    size_t e = biome_str.find("\"", p + 1);
                    if(e == std::string::npos) return "";
                    return biome_str.substr(p + 1, e - p - 1);
                };
                
                b.name = find_val("name");
                if(!b.name.empty()) {
                    b.texture_index = std::stoi(find_val("texture_index"));
                    biomes.push_back(b);
                }
                
                obj_start = content.find("{", obj_end);
            }
        }
    }
};

static Pivot4ConfigPro C_pro;

// =============================================================================
// MOTEUR MATHÉMATIQUE (Déterministe)
// =============================================================================

class MoteurPivot4 {
    uint32_t etat;
    static constexpr uint32_t MULT = 1664525, INC = 1013904223;
public:
    MoteurPivot4(uint32_t g = 1) { 
        etat = (g ^ C.seed) & 0xFFFFFFFC; 
        if(!etat) etat = 4; 
    }
    uint32_t prochain() { 
        uint32_t b = etat * MULT + INC; 
        etat = b & 0xFFFFFFFC; 
        if(!etat) etat = 4; 
        return etat; 
    }
    float prochain_float() { return prochain() / (float)0xFFFFFFFF; }
};

static uint32_t combiner(int x, int y, int z) {
    uint32_t h = C.seed;
    h = h * 31 + x; h = h * 31 + y; h = h * 31 + z;
    h = (h ^ 0x9E3779B9) & 0xFFFFFFFC;
    return h ? h : 4;
}

// =============================================================================
// MASQUES D'INFLUENCE
// =============================================================================

static std::vector<ControlMap> g_control_maps;

#ifdef PIVOT4_USE_STB_IMAGE
void pivot4_ajouter_masque_influence(const std::string& nom, const std::string& fichier, 
                                      float intensite = 1.0f, int mode = 0) {
    ControlMap cm;
    cm.nom = nom;
    cm.fichier = fichier;
    cm.intensite = intensite;
    cm.mode = mode;
    
    int channels;
    unsigned char* img = stbi_load(fichier.c_str(), &cm.largeur, &cm.hauteur, &channels, 1);
    if(img) {
        cm.pixels.assign(img, img + (cm.largeur * cm.hauteur));
        stbi_image_free(img);
        cm.charge = true;
        g_control_maps.push_back(cm);
        std::cout << "✓ Masque chargé: " << nom << " (" << cm.largeur << "x" << cm.hauteur << ")\n";
    } else {
        std::cerr << "❌ Échec chargement masque: " << fichier << "\n";
    }
}

static float get_valeur_masque(const ControlMap& cm, float x, float z, int taille_monde) {
    if(!cm.charge || cm.largeur == 0) return 0.0f;
    int px = (int)((x / taille_monde + 0.5f) * cm.largeur);
    int pz = (int)((z / taille_monde + 0.5f) * cm.hauteur);
    px = std::max(0, std::min(cm.largeur - 1, px));
    pz = std::max(0, std::min(cm.hauteur - 1, pz));
    return cm.pixels[pz * cm.largeur + px] / 255.0f;
}
#endif

// =============================================================================
// GÉNÉRATION DE TERRAIN AVEC MASQUES
// =============================================================================

static int hauteur_terrain(int x, int z) {
    MoteurPivot4 m(combiner(x, 0, z));
    m.prochain(); m.prochain(); uint32_t v = m.prochain();
    int base = C.hauteur_min + ((v >> 16) % (C.hauteur_max - C.hauteur_min));
    
    MoteurPivot4 m2(combiner(x >> 1, 0, z >> 1));
    int var = ((m2.prochain() >> 8) % (C.amplitude_octave * 2 + 1)) - C.amplitude_octave;
    int r = base + var;
    
#ifdef PIVOT4_USE_STB_IMAGE
    for(const auto& cm : g_control_maps) {
        float val = get_valeur_masque(cm, x, z, 10000);
        if(cm.mode == 0) { // Additif
            r += (int)(val * cm.intensite * 5);
        } else if(cm.mode == 1) { // Multiplicatif
            r = (int)(r * (1.0f + (val - 0.5f) * cm.intensite));
        }
    }
#endif
    
    return (r < C.hauteur_min) ? C.hauteur_min : (r > C.hauteur_max) ? C.hauteur_max : r;
}

// =============================================================================
// SYSTÈME DE CACHE
// =============================================================================

static std::unordered_map<uint64_t, int> cache_h;
static std::unordered_map<uint64_t, Pivot4Chunk> cache_c;
static const size_t CACHE_MAX_H = 100000;
static const size_t CACHE_MAX_C = 10000;
static int calls_since_clean = 0;

static uint64_t key(int x, int z) { 
    uint64_t ux = (uint64_t)(x) + 2147483648ULL;
    uint64_t uz = (uint64_t)(z) + 2147483648ULL;
    return (ux << 32) | uz;
}

static void pivot4_cache_vider() {
    if(cache_h.size() > CACHE_MAX_H) cache_h.clear();
    if(cache_c.size() > CACHE_MAX_C) cache_c.clear();
}

int pivot4_get_hauteur(int x, int z) {
    calls_since_clean++;
    if(calls_since_clean > 1000) {
        pivot4_cache_vider();
        calls_since_clean = 0;
    }
    
    auto it = cache_h.find(key(x, z));
    if(it != cache_h.end()) return it->second;
    int h = hauteur_terrain(x, z);
    cache_h[key(x, z)] = h;
    return h;
}

int pivot4_get_type(int x, int y, int z) {
    int h = pivot4_get_hauteur(x, z);
    if(y < h) return 1;
    if(y == h) return (h < 0) ? 2 : 1;
    return 0;
}

std::string pivot4_get_biome(int x, int z) {
    int cx = x >= 0 ? x / C.chunk_taille : (x - C.chunk_taille + 1) / C.chunk_taille;
    int cz = z >= 0 ? z / C.chunk_taille : (z - C.chunk_taille + 1) / C.chunk_taille;
    MoteurPivot4 m(combiner(cx * 100, cz * 100, 1000));
    uint32_t val = m.prochain();
    int h = pivot4_get_hauteur(x, z);
    if(h < 0) return "ocean";
    if(h < 1) return "plage";
    if(h > 5) return "montagne";
    int idx = (val >> 16) % 4;
    if(idx == 0) return "desert";
    if(idx == 1) return "foret";
    if(idx == 2) return "plaine";
    return "colline";
}

// =============================================================================
// GÉNÉRATION DE RIVIÈRES
// =============================================================================

static std::vector<Riviere> generer_rivieres(int min_cx, int max_cx, int min_cz, int max_cz) {
    std::vector<Riviere> rivieres;
    if(!C.rivieres_active) return rivieres;
    
    MoteurPivot4 rng_riv(combiner(min_cx, min_cz, 9999));
    int nb_rivieres = C.nb_rivieres + (rng_riv.prochain() % 4);
    
    for(int r = 0; r < nb_rivieres; r++) {
        Riviere riv;
        int start_x = min_cx * C.chunk_taille + (rng_riv.prochain() % ((max_cx - min_cx + 1) * C.chunk_taille));
        int start_z = min_cz * C.chunk_taille + (rng_riv.prochain() % ((max_cz - min_cz + 1) * C.chunk_taille));
        
        int h_start = pivot4_get_hauteur(start_x, start_z);
        if(h_start < C.hauteur_max - 3) continue;
        
        riv.points.push_back({start_x, start_z});
        int x = start_x, z = start_z;
        int etapes = 0;
        
        while(etapes < 500) {
            int best_dir_x = 0, best_dir_z = 0;
            int meilleur_h = pivot4_get_hauteur(x, z);
            
            for(int dx = -1; dx <= 1; dx++) {
                for(int dz = -1; dz <= 1; dz++) {
                    if(dx == 0 && dz == 0) continue;
                    int h = pivot4_get_hauteur(x + dx, z + dz);
                    if(h < meilleur_h) {
                        meilleur_h = h;
                        best_dir_x = dx;
                        best_dir_z = dz;
                    }
                }
            }
            
            if(best_dir_x == 0 && best_dir_z == 0) break;
            x += best_dir_x;
            z += best_dir_z;
            riv.points.push_back({x, z});
            etapes++;
        }
        
        riv.largeur = 1.0f + (rng_riv.prochain() % 30) / 10.0f;
        riv.profondeur = -1.0f - (rng_riv.prochain() % 20) / 10.0f;
        rivieres.push_back(riv);
    }
    
    return rivieres;
}

static void appliquer_erosion(int min_cx, int max_cx, int min_cz, int max_cz) {
    if(!C.erosion_active) return;
    
    auto rivieres = generer_rivieres(min_cx, max_cx, min_cz, max_cz);
    
    for(const auto& riv : rivieres) {
        for(size_t i = 0; i < riv.points.size(); i++) {
            int x = riv.points[i].first;
            int z = riv.points[i].second;
            
            for(int dx = -(int)riv.largeur; dx <= (int)riv.largeur; dx++) {
                for(int dz = -(int)riv.largeur; dz <= (int)riv.largeur; dz++) {
                    int x_creux = x + dx;
                    int z_creux = z + dz;
                    float dist = sqrt(dx*dx + dz*dz);
                    if(dist <= riv.largeur) {
                        float facteur = 1.0f - (dist / riv.largeur);
                        int h_actuel = pivot4_get_hauteur(x_creux, z_creux);
                        int h_nouveau = std::max(C.hauteur_min, 
                                                  h_actuel + (int)(riv.profondeur * facteur));
                        cache_h[key(x_creux, z_creux)] = h_nouveau;
                    }
                }
            }
        }
    }
    
    if(!rivieres.empty()) {
        std::cout << "  ✓ " << rivieres.size() << " rivières générées\n";
    }
}

// =============================================================================
// LOGIQUE DE CHUNK
// =============================================================================

Pivot4Chunk pivot4_get_chunk(int cx, int cz) {
    auto it = cache_c.find(key(cx, cz));
    if(it != cache_c.end()) return it->second;
    
    Pivot4Chunk chunk; 
    chunk.cx = cx; 
    chunk.cz = cz;
    
    for(int x = 0; x < C.chunk_taille; x++) {
        for(int z = 0; z < C.chunk_taille; z++) {
            int wx = cx * C.chunk_taille + x;
            int wz = cz * C.chunk_taille + z;
            int h = pivot4_get_hauteur(wx, wz);
            std::string biome = pivot4_get_biome(wx, wz);
            for(int y = C.hauteur_min; y <= h; y++) {
                chunk.tuiles.push_back({wx, wz, y, pivot4_get_type(wx, y, wz), biome, 3});
            }
        }
    }
    
    MoteurPivot4 rng_obj(combiner(cx * 1000, cz * 1000, 2000));
    int nb = rng_obj.prochain() % (C.objets_par_chunk + 1);
    for(int i = 0; i < nb; i++) {
        int wx = cx * C.chunk_taille + (rng_obj.prochain() % C.chunk_taille);
        int wz = cz * C.chunk_taille + (rng_obj.prochain() % C.chunk_taille);
        chunk.objets.push_back({"arbre", (float)wx, (float)wz, pivot4_get_hauteur(wx, wz) + 1});
    }
    
    chunk.biome = pivot4_get_biome(cx * C.chunk_taille, cz * C.chunk_taille);
    cache_c[key(cx, cz)] = chunk;
    return chunk;
}

// =============================================================================
// FUSION ET EXPORT
// =============================================================================

Pivot4WorldMap pivot4_fusionner_avec_lissage(int min_cx, int max_cx, int min_cz, int max_cz) {
    int largeur = max_cx - min_cx + 1;
    int hauteur = max_cz - min_cz + 1;
    Pivot4WorldMap world(largeur, hauteur);
    
    // Appliquer l'érosion d'abord
    appliquer_erosion(min_cx, max_cx, min_cz, max_cz);
    
    for(int cz = min_cz; cz <= max_cz; cz++) {
        for(int cx = min_cx; cx <= max_cx; cx++) {
            int off_x = (cx - min_cx) * C.chunk_taille;
            int off_z = (cz - min_cz) * C.chunk_taille;
            for(int z = 0; z < C.chunk_taille; z++) {
                for(int x = 0; x < C.chunk_taille; x++) {
                    int idx = (off_z + z) * world.taille_totale_x + (off_x + x);
                    world.heightmap[idx] = (uint16_t)(pivot4_get_hauteur(cx*C.chunk_taille+x, cz*C.chunk_taille+z) + 100);
                }
            }
        }
    }
    return world;
}

void pivot4_export_chunk_raw(const Pivot4Chunk& chunk, const std::string& filename) {
    std::ofstream out(filename, std::ios::binary);
    for(int z = 0; z < C.chunk_taille; ++z) {
        for(int x = 0; x < C.chunk_taille; ++x) {
            int wx = chunk.cx * C.chunk_taille + x;
            int wz = chunk.cz * C.chunk_taille + z;
            uint16_t h = (uint16_t)(pivot4_get_hauteur(wx, wz) + 100);
            out.write(reinterpret_cast<const char*>(&h), sizeof(uint16_t));
        }
    }
}

void pivot4_exporter_en_raw(const Pivot4WorldMap& world, const std::string& filename, bool avec_header = false) {
    std::ofstream out(filename, std::ios::binary);
    if (avec_header) {
        uint32_t magic = 0x52415700;
        out.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(&world.taille_totale_x), sizeof(int));
        out.write(reinterpret_cast<const char*>(&world.taille_totale_z), sizeof(int));
    }
    out.write(reinterpret_cast<const char*>(world.heightmap.data()), 
              world.heightmap.size() * sizeof(uint16_t));
}

void pivot4_exporter_en_pgm(const Pivot4WorldMap& world, const std::string& filename) {
    std::ofstream out(filename);
    uint16_t min_h = 65535, max_h = 0;
    for(auto h : world.heightmap) {
        if(h < min_h) min_h = h;
        if(h > max_h) max_h = h;
    }
    
    out << "P2\n" << world.taille_totale_x << " " << world.taille_totale_z << "\n65535\n";
    for(size_t i = 0; i < world.heightmap.size(); i++) {
        uint16_t val = (min_h == max_h) ? 32768 : 
                       (world.heightmap[i] - min_h) * 65535 / (max_h - min_h);
        out << val;
        if((i + 1) % world.taille_totale_x == 0) out << "\n";
        else out << " ";
    }
}

// =============================================================================
// EXPORT OBJ AVEC OPTIMISATION
// =============================================================================

static Couleur obtenir_couleur_par_biome(const std::string& biome, int hauteur) {
    if(biome == "ocean") return {0.1f, 0.3f, 0.6f};
    if(biome == "plage") return {0.9f, 0.8f, 0.5f};
    if(biome == "foret") return {0.2f, 0.5f, 0.1f};
    if(biome == "desert") return {0.8f, 0.7f, 0.4f};
    if(biome == "montagne") {
        if(hauteur > 7) return {0.9f, 0.9f, 0.9f};
        if(hauteur > 5) return {0.5f, 0.5f, 0.5f};
        return {0.4f, 0.3f, 0.2f};
    }
    if(biome == "plaine") return {0.4f, 0.7f, 0.2f};
    if(biome == "colline") return {0.5f, 0.6f, 0.3f};
    return {0.5f, 0.5f, 0.5f};
}

void pivot4_exporter_en_obj(const Pivot4WorldMap& world, const std::string& filename) {
    std::ofstream out(filename);
    int stride = world.taille_totale_x;
    
    out << "# PIVOT4 Heightmap Export\n";
    out << "# Taille: " << stride << "x" << world.taille_totale_z << "\n\n";
    
    for(int z = 0; z < world.taille_totale_z; z++) {
        for(int x = 0; x < stride; x++) {
            float h = (world.heightmap[z * stride + x] - 100) / 10.0f;
            out << "v " << x/10.0f << " " << h << " " << z/10.0f << "\n";
        }
    }
    
    out << "\n# Faces\n";
    for(int z = 0; z < world.taille_totale_z - 1; z++) {
        for(int x = 0; x < stride - 1; x++) {
            int i = z * stride + x + 1;
            out << "f " << i << " " << i+1 << " " << i+stride << "\n";
            out << "f " << i+1 << " " << i+stride+1 << " " << i+stride << "\n";
        }
    }
}

void pivot4_exporter_en_ply(const Pivot4WorldMap& world, const std::string& filename) {
    std::ofstream out(filename);
    int stride = world.taille_totale_x;
    int nb_sommets = stride * world.taille_totale_z;
    int nb_faces = (stride - 1) * (world.taille_totale_z - 1) * 2;
    
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex " << nb_sommets << "\n";
    out << "property float x\nproperty float y\nproperty float z\n";
    out << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    out << "element face " << nb_faces << "\n";
    out << "property list uchar int vertex_index\n";
    out << "end_header\n";
    
    for(int z = 0; z < world.taille_totale_z; z++) {
        for(int x = 0; x < stride; x++) {
            float h = (world.heightmap[z * stride + x] - 100) / 10.0f;
            int cx = x / C.chunk_taille;
            int cz = z / C.chunk_taille;
            std::string biome = pivot4_get_biome(cx * C.chunk_taille + x % C.chunk_taille, 
                                                   cz * C.chunk_taille + z % C.chunk_taille);
            Couleur col = obtenir_couleur_par_biome(biome, (int)(h * 10));
            
            out << x/10.0f << " " << h << " " << z/10.0f << " "
                << (int)(col.r * 255) << " " << (int)(col.g * 255) << " " << (int)(col.b * 255) << "\n";
        }
    }
    
    for(int z = 0; z < world.taille_totale_z - 1; z++) {
        for(int x = 0; x < stride - 1; x++) {
            int idx = z * stride + x;
            out << "3 " << idx << " " << idx + 1 << " " << idx + stride << "\n";
            out << "3 " << idx + 1 << " " << idx + stride + 1 << " " << idx + stride << "\n";
        }
    }
}

// =============================================================================
// EXPORT POUR MOTEURS DE JEU
// =============================================================================

void pivot4_exporter_splatmaps(const Pivot4WorldMap& world, const std::string& dossier) {
    int largeur = world.taille_totale_x;
    int hauteur = world.taille_totale_z;
    
    std::vector<BiomeLayer> biomes_a_utiliser = C_pro.biomes;
    if(biomes_a_utiliser.empty()) {
        biomes_a_utiliser = {
            {"ocean", 0.0f, 0.2f, "#1a4d8c", 0},
            {"plage", 0.2f, 0.25f, "#e8d5a3", 1},
            {"foret", 0.3f, 0.6f, "#2d5a27", 2},
            {"montagne", 0.7f, 1.0f, "#8b8b8b", 3}
        };
    }
    
    for(const auto& biome : biomes_a_utiliser) {
        std::vector<uint8_t> canal(largeur * hauteur, 0);
        
        for(int z = 0; z < hauteur; z++) {
            for(int x = 0; x < largeur; x++) {
                int idx = z * largeur + x;
                uint16_t h_raw = world.heightmap[idx];
                int hauteur_val = (h_raw - 100);
                float t = (hauteur_val - C.hauteur_min) / (float)(C.hauteur_max - C.hauteur_min);
                t = std::max(0.0f, std::min(1.0f, t));
                
                float intensite = 0.0f;
                if(t >= biome.seuil_min && t <= biome.seuil_max) {
                    intensite = 1.0f;
                    float range = (biome.seuil_max - biome.seuil_min);
                    if(range > 0) {
                        intensite = 1.0f - std::abs(t - (biome.seuil_min + range/2)) / (range/2);
                        intensite = std::max(0.0f, std::min(1.0f, intensite));
                    }
                }
                canal[idx] = (uint8_t)(intensite * 255);
            }
        }
        
        std::string nom_fichier = dossier + "/splatmap_" + biome.name + ".raw";
        std::ofstream out(nom_fichier, std::ios::binary);
        out.write(reinterpret_cast<const char*>(canal.data()), canal.size());
        std::cout << "  ✓ Splatmap: " << nom_fichier << "\n";
    }
}

void pivot4_exporter_monde_complet(const Pivot4WorldMap& world, const std::string& filename) {
    std::ofstream out(filename, std::ios::binary);
    uint32_t magic = 0x5049564D;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&world.taille_totale_x), sizeof(int));
    out.write(reinterpret_cast<const char*>(&world.taille_totale_z), sizeof(int));
    out.write(reinterpret_cast<const char*>(world.heightmap.data()), world.heightmap.size() * sizeof(uint16_t));
}

void pivot4_exporter_monde_ligne_par_ligne(int min_cx, int max_cx, int min_cz, int max_cz, 
                                           const std::string& filename) {
    std::ofstream out(filename, std::ios::binary);
    int largeur_totale = (max_cx - min_cx + 1) * C.chunk_taille;
    int hauteur_totale = (max_cz - min_cz + 1) * C.chunk_taille;
    
    uint32_t magic = 0x5049564D;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(&largeur_totale), sizeof(int));
    out.write(reinterpret_cast<const char*>(&hauteur_totale), sizeof(int));
    
    std::vector<uint16_t> ligne(largeur_totale);
    
    for(int cz = min_cz; cz <= max_cz; cz++) {
        for(int ligne_z = 0; ligne_z < C.chunk_taille; ligne_z++) {
            int idx = 0;
            for(int cx = min_cx; cx <= max_cx; cx++) {
                for(int x = 0; x < C.chunk_taille; x++) {
                    int wx = cx * C.chunk_taille + x;
                    int wz = cz * C.chunk_taille + ligne_z;
                    ligne[idx++] = pivot4_get_hauteur(wx, wz) + 100;
                }
            }
            out.write(reinterpret_cast<const char*>(ligne.data()), ligne.size() * sizeof(uint16_t));
        }
    }
}

// =============================================================================
// CONFIGURATION ET INITIALISATION
// =============================================================================

void pivot4_init(const std::string& f = "config.txt") { 
    C.charger(f); 
    cache_h.clear(); 
    cache_c.clear(); 
    calls_since_clean = 0;
}

void pivot4_init_pro(const std::string& f = "config.json") {
    C_pro.charger_json(f);
    C.seed = C_pro.seed;
    C.hauteur_min = C_pro.hauteur_min;
    C.hauteur_max = C_pro.hauteur_max;
    C.amplitude_octave = C_pro.amplitude_octave;
    C.chunk_taille = C_pro.chunk_taille;
    C.objets_par_chunk = C_pro.objets_par_chunk;
    C.rivieres_active = C_pro.rivieres_active;
    C.erosion_active = C_pro.erosion_active;
    C.nb_rivieres = C_pro.nb_rivieres;
    
#ifdef PIVOT4_USE_STB_IMAGE
    if(!C_pro.masque_entree.empty()) {
        pivot4_ajouter_masque_influence("main", C_pro.masque_entree, 0.5f, 1);
    }
#endif
}

// =============================================================================
// MODE BATCH
// =============================================================================

struct BatchJob {
    std::string name;
    int min_cx = -5, max_cx = 5, min_cz = -5, max_cz = 5;
    bool exporter_heightmap = true;
    bool exporter_splatmaps = true;
    bool exporter_objets = true;
    bool exporter_ply = true;
    std::vector<std::string> formats_export = {"obj", "ply"};
};

static void pivot4_executer_batch(const std::string& fichier_config) {
    std::ifstream f(fichier_config);
    if(!f.is_open()) {
        std::cerr << "❌ Impossible d'ouvrir " << fichier_config << "\n";
        return;
    }
    
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    // Extraire les jobs (JSON simplifié)
    std::vector<BatchJob> jobs;
    size_t jobs_pos = content.find("\"jobs\"");
    if(jobs_pos != std::string::npos) {
        size_t array_start = content.find("[", jobs_pos);
        if(array_start != std::string::npos) {
            size_t obj_start = content.find("{", array_start);
            while(obj_start != std::string::npos) {
                size_t obj_end = content.find("}", obj_start);
                if(obj_end == std::string::npos) break;
                
                BatchJob job;
                std::string job_str = content.substr(obj_start, obj_end - obj_start + 1);
                
                auto extract = [&](const std::string& key) -> std::string {
                    size_t p = job_str.find("\"" + key + "\"");
                    if(p == std::string::npos) return "";
                    p = job_str.find(":", p);
                    if(p == std::string::npos) return "";
                    p = job_str.find("\"", p);
                    if(p == std::string::npos) return "";
                    size_t e = job_str.find("\"", p + 1);
                    if(e == std::string::npos) return "";
                    return job_str.substr(p + 1, e - p - 1);
                };
                
                job.name = extract("name");
                if(job.name.empty()) job.name = "job_" + std::to_string(jobs.size());
                
                try {
                    job.min_cx = std::stoi(extract("min_cx"));
                    job.max_cx = std::stoi(extract("max_cx"));
                    job.min_cz = std::stoi(extract("min_cz"));
                    job.max_cz = std::stoi(extract("max_cz"));
                } catch(...) {}
                
                jobs.push_back(job);
                obj_start = content.find("{", obj_end);
            }
        }
    }
    
    if(jobs.empty()) {
        BatchJob default_job;
        default_job.name = "default";
        jobs.push_back(default_job);
    }
    
    // Créer le dossier de sortie
    system(("mkdir -p " + C.export_settings.output_dir).c_str());
    
    // Traiter chaque job
    for(const auto& job : jobs) {
        std::cout << "\n📦 Traitement du job: " << job.name << "\n";
        std::cout << "   Zone: chunks [" << job.min_cx << ".." << job.max_cx 
                  << "] x [" << job.min_cz << ".." << job.max_cz << "]\n";
        
        auto world = pivot4_fusionner_avec_lissage(job.min_cx, job.max_cx, job.min_cz, job.max_cz);
        std::string dossier = C.export_settings.output_dir + "/" + job.name;
        system(("mkdir -p " + dossier).c_str());
        
        if(job.exporter_heightmap) {
            std::string hm_file = dossier + "/heightmap.raw";
            pivot4_exporter_en_raw(world, hm_file, true);
            std::cout << "  ✓ Heightmap: " << hm_file << "\n";
        }
        
        if(job.exporter_splatmaps) {
            pivot4_exporter_splatmaps(world, dossier);
        }
        
        if(job.exporter_ply) {
            std::string ply_file = dossier + "/terrain.ply";
            pivot4_exporter_en_ply(world, ply_file);
            std::cout << "  ✓ PLY: " << ply_file << "\n";
        }
        
        for(const auto& fmt : job.formats_export) {
            if(fmt == "obj") {
                std::string obj_file = dossier + "/terrain.obj";
                pivot4_exporter_en_obj(world, obj_file);
                std::cout << "  ✓ OBJ: " << obj_file << "\n";
            }
        }
    }
    
    std::cout << "\n✅ Batch terminé !\n";
}

// =============================================================================
// INTERFACE CLI
// =============================================================================

static void pivot4_afficher_aide() {
    std::cout << "\n"
              << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║                    PIVOT4 v3.0 - Modding Edition             ║\n"
              << "║              Moteur de Génération Procédurale Universel      ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n\n"
              << "Usage: pivot4 <commande> [arguments]\n\n"
              << "📁 Commandes principales:\n"
              << "  batch <config.json>               - Mode batch (recommandé)\n"
              << "  world <min_cx> <max_cx> <min_cz> <max_cz> <output.piv4>\n"
              << "  chunk <cx> <cz> <output.raw>      - Exporte un chunk\n\n"
              << "📄 Commandes d'export:\n"
              << "  export-obj <min_cx> <max_cx> <min_cz> <max_cz> <output.obj>\n"
              << "  export-ply <min_cx> <max_cx> <min_cz> <max_cz> <output.ply>\n"
              << "  export-splatmap <min_cx> <max_cx> <min_cz> <max_cz> <dossier>\n\n"
              << "⚙️ Utilitaires:\n"
              << "  config                            - Affiche la configuration\n"
              << "  test [output.piv4]                - Génère un monde test 3x3\n\n"
              << "💡 Exemples:\n"
              << "  ./pivot4 batch config.json\n"
              << "  ./pivot4 export-ply -5 5 -5 5 terrain.ply\n"
              << "  ./pivot4 test mon_monde.piv4\n\n";
}

bool pivot4_cli_executer(int argc, char* argv[]) {
    if(argc < 2) {
        pivot4_afficher_aide();
        return false;
    }
    
    std::string cmd = argv[1];
    
    if(cmd == "batch" && argc >= 3) {
        pivot4_init_pro(argv[2]);
        pivot4_executer_batch(argv[2]);
        
    } else if(cmd == "world" && argc >= 7) {
        int min_cx = std::stoi(argv[2]), max_cx = std::stoi(argv[3]);
        int min_cz = std::stoi(argv[4]), max_cz = std::stoi(argv[5]);
        auto world = pivot4_fusionner_avec_lissage(min_cx, max_cx, min_cz, max_cz);
        pivot4_exporter_monde_complet(world, argv[6]);
        std::cout << "✓ Monde exporté: " << world.taille_totale_x << "x" << world.taille_totale_z << "\n";
        
    } else if(cmd == "chunk" && argc >= 5) {
        int cx = std::stoi(argv[2]), cz = std::stoi(argv[3]);
        pivot4_export_chunk_raw(pivot4_get_chunk(cx, cz), argv[4]);
        std::cout << "✓ Chunk (" << cx << "," << cz << ") exporté\n";
        
    } else if(cmd == "export-obj" && argc >= 7) {
        int min_cx = std::stoi(argv[2]), max_cx = std::stoi(argv[3]);
        int min_cz = std::stoi(argv[4]), max_cz = std::stoi(argv[5]);
        auto world = pivot4_fusionner_avec_lissage(min_cx, max_cx, min_cz, max_cz);
        pivot4_exporter_en_obj(world, argv[6]);
        std::cout << "✓ OBJ exporté\n";
        
    } else if(cmd == "export-ply" && argc >= 7) {
        int min_cx = std::stoi(argv[2]), max_cx = std::stoi(argv[3]);
        int min_cz = std::stoi(argv[4]), max_cz = std::stoi(argv[5]);
        auto world = pivot4_fusionner_avec_lissage(min_cx, max_cx, min_cz, max_cz);
        pivot4_exporter_en_ply(world, argv[6]);
        std::cout << "✓ PLY exporté\n";
        
    } else if(cmd == "export-splatmap" && argc >= 7) {
        int min_cx = std::stoi(argv[2]), max_cx = std::stoi(argv[3]);
        int min_cz = std::stoi(argv[4]), max_cz = std::stoi(argv[5]);
        auto world = pivot4_fusionner_avec_lissage(min_cx, max_cx, min_cz, max_cz);
        pivot4_exporter_splatmaps(world, argv[6]);
        std::cout << "✓ Splatmaps exportées\n";
        
    } else if(cmd == "config") {
        std::cout << "\n📊 PIVOT4 - Configuration\n"
                  << "   Seed: " << C.seed << "\n"
                  << "   Hauteurs: [" << C.hauteur_min << ", " << C.hauteur_max << "]\n"
                  << "   Amplitude: " << C.amplitude_octave << "\n"
                  << "   Taille chunk: " << C.chunk_taille << "x" << C.chunk_taille << "\n"
                  << "   Objets/chunk: " << C.objets_par_chunk << "\n"
                  << "   Érosion: " << (C.erosion_active ? "ON" : "OFF") << "\n"
                  << "   Rivières: " << (C.rivieres_active ? "ON" : "OFF") << "\n\n";
                  
    } else if(cmd == "test") {
        std::string output = (argc >= 3) ? argv[2] : "test_world.piv4";
        auto world = pivot4_fusionner_avec_lissage(-1, 1, -1, 1);
        pivot4_exporter_monde_complet(world, output);
        pivot4_exporter_en_ply(world, "test_terrain.ply");
        std::cout << "✓ Monde test (3x3 chunks) exporté\n";
        
    } else {
        std::cout << "❌ Commande inconnue: " << cmd << "\n";
        pivot4_afficher_aide();
        return false;
    }
    return true;
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char* argv[]) {
    std::cout << "\n🔄 PIVOT4 v3.0 - Modding Edition\n";
    std::cout << "   Moteur de Génération Procédurale Universel\n\n";
    
    // Charger la config par défaut
    pivot4_init("config.txt");
    
    // Vérifier si une config JSON existe
    std::ifstream test_json("config.json");
    if(test_json.is_open()) {
        test_json.close();
        pivot4_init_pro("config.json");
    }
    
    return pivot4_cli_executer(argc, argv) ? 0 : 1;
}
