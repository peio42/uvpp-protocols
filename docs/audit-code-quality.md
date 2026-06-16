# Audit qualité de code — uvpp-protocols
_(Temporary working file - sorry for the French)_

Cet audit couvre l'écriture, la cohérence, la lisibilité et la correction du
code. Il est distinct de l'audit API (`audit-api.md`) ; les deux se
complètent.

---

## 1. Bug : référence pendante après déplacement de `server`

**Fichier :** `src/http/server.cpp`

`server::impl` stocke une référence `server& owner` initialisée dans le
constructeur de `server` :

```cpp
server::server(uv::loop& loop, server_options options)
    : loop_(&loop), options_(options), impl_(std::make_unique<impl>(*this)) {
```

Le constructeur de déplacement est `= default`, ce qui déplace `impl_` sans
mettre à jour `impl_->owner`. Après un déplacement, `impl_->owner` référence
l'ancien objet `server` (moved-from). Tous les accès ultérieurs à
`owner_.owner.router_`, `owner_.owner.options_`, etc. depuis les sessions
actives sont un comportement indéfini.

**Correctif :** définir le constructeur de déplacement explicitement pour
mettre à jour `impl_->owner = *this`, ou supprimer le déplacement.

---

## 2. WebSocket : responsabilité explicite du pong

**Fichier :** `src/websocket/session.cpp`

Ce point a été corrigé avec `accept_options::auto_pong(bool)`.

```cpp
void dispatch_ping(std::span<const std::byte> payload) {
    auto handle = session{shared_from_this()};
    if (options.on_ping()) {
        options.on_ping()(handle, payload);  // user callback
    }
    if (options.auto_pong()) {
        send_frame(opcode::pong, payload);
    }
}
```

Par défaut, la session répond automatiquement aux ping avec un pong conforme ;
`on_ping` est alors un hook d'observation. Les utilisateurs avancés peuvent
appeler `.auto_pong(false)` et prennent alors explicitement la responsabilité
de répondre depuis `on_ping`.

---

## 3. SHA-1 implémenté manuellement

**Fichier :** `src/websocket/session.cpp` (lignes 97–183)

L'implémentation SHA-1 pour le handshake WebSocket est entièrement codée à la
main, en 80+ lignes de manipulations de bits. Cette fonction est
security-sensitive (RFC 6455 § 4.1), non testée indépendamment, et constitue
un risque de maintenance. Une implémentation incorrecte passerait le test
d'intégration si l'autre côté de la connexion est indulgent.

**Correctif :** utiliser une implémentation de la bibliothèque standard
(`<openssl/sha.h>`, ou une dépendance légère déjà présente). À défaut,
extraire et tester unitairement cette fonction.

---

## 4. `read_buffer` : `erase` au début d'un `std::vector`

**Fichier :** `src/websocket/session.cpp`

```cpp
read_buffer.erase(
    read_buffer.begin(),
    read_buffer.begin() + static_cast<std::ptrdiff_t>(offset + payload.size()));
```

Effacer les éléments traités au début d'un `std::vector` provoque un décalage
de toutes les données restantes : coût O(n) à chaque frame. Avec des messages
de grande taille ou un flux dense, cela génère une pression mémoire et CPU
significative.

**Correctif :** utiliser un `std::deque<std::byte>` avec `pop_front` ou un
buffer circulaire, ou maintenir un index de lecture sans réallouer.

---

## 5. `escape_json` dupliqué trois fois

**Fichiers :** `src/http/response.cpp`, `examples/log_streaming.cpp`,
`examples/local_json_api.cpp`

La même fonction d'échappement JSON est définie trois fois, sous deux noms
différents (`escape_json_string` et `escape_json`), avec des différences
subtiles : `response.cpp` gère `\b` et `\f`, les deux exemples ne les gèrent
pas.

```cpp
// response.cpp
case '\b': escaped += "\\b"; break;
case '\f': escaped += "\\f"; break;
// absent dans log_streaming.cpp et local_json_api.cpp
```

**Correctif :** extraire une fonction partagée dans un header utilitaire.

---

## 6. `as_bytes()` défini dans deux unités de traduction

**Fichiers :** `src/http/server.cpp`, `examples/log_streaming.cpp`

```cpp
// server.cpp
std::span<const std::byte> as_bytes(std::string& value) noexcept { ... }

// log_streaming.cpp
std::span<const std::byte> as_bytes(const std::string& value) noexcept { ... }
```

La même conversion est redéfinie deux fois. En C++20, `std::as_bytes` couvre
ce cas. À défaut, une version commune dans un header utilitaire éviterait
cette duplication.

---

## 7. Convention de nommage des membres incohérente

Le code alterne entre deux conventions pour les membres de structs/classes
internes :

- `server::impl::session` : suffixe underscore (`writing_`, `closed_`,
  `writes_`, `responses_`, `parser_`…)
- `websocket::session::state` : pas de suffixe (`writing`, `closed`,
  `writes`, `read_buffer`…)

Les deux structs jouent un rôle symétrique (état interne d'une session) et
appliquent des conventions opposées. La cohérence devrait s'appliquer à
l'ensemble du projet.

---

## 8. Variable locale `on_write` qui occulte la méthode `on_write`

**Fichier :** `src/websocket/session.cpp`

```cpp
void on_write(uvp::io::stream_error error) {        // méthode
    // ...
    auto on_write = !writes.empty()                 // variable locale du même nom
        ? std::move(writes.front().on_write)
        : uvp::io::write_callback{};
    // ...
    if (on_write) { on_write({}); }                 // appel de la variable
}
```

La variable locale `on_write` porte le même nom que la méthode courante.
Bien que correct en l'état (les appels sont bien distincts), c'est une source
de confusion lors de la lecture du code.

---

## 9. `header_name_equals` crée un objet `headers` temporaire

**Fichier :** `src/http/server.cpp`

```cpp
bool header_name_equals(std::string_view name, std::string_view expected) noexcept {
    return headers{}.set(name, "").contains(expected);
}
```

Pour comparer deux noms de header insensibles à la casse, cette fonction
alloue un `headers` complet (avec son `vector<pair<string,string>>`), insère
une entrée, puis vérifie la présence. La raison est que `headers::names_equal`
est privé. Le correctif serait de rendre `names_equal` accessible (méthode
statique publique ou fonction libre) pour éviter cette allocation.

---

## 10. `reason_phrase_for` duplique la logique de `reason_phrase`

**Fichiers :** `src/http/server.cpp`, `src/http/status.cpp`

```cpp
// server.cpp – prend un entier, retourne std::string
std::string reason_phrase_for(unsigned int status_code) {
    switch (status_code) {
    case 200: return std::string(reason_phrase(status::ok));
    // ...
    }
}

// status.cpp – prend l'enum, retourne std::string_view
std::string_view reason_phrase(status value) noexcept { ... }
```

La version de `server.cpp` enveloppe celle de `status.cpp` mais duplique
tout le `switch`. Le code devrait juste caster l'entier en `status` et appeler
`reason_phrase`, ou étendre `reason_phrase` pour accepter un entier.

---

## 11. `http1_event` transporte toujours les deux champs

**Fichier :** `src/http/detail/http1_state_machine.hpp`

```cpp
struct http1_event {
    type event_type = type::complete;
    http1_message message;   // vide pour les events body
    std::string body;        // vide pour les events headers/complete
};
```

Pour les événements de type `body`, `message` est un `http1_message`
default-construit (avec un `headers` vide et des strings allouées). Pour les
événements `headers` et `complete`, `body` est une string vide. Chaque
événement porte systématiquement des données inutiles.

**Correctif :** `std::variant<http1_message, std::string>`, ou des types
d'événements distincts.

---

## 12. Nombre magique `return 2` dans `on_headers_complete`

**Fichier :** `src/http/detail/http1_state_machine.cpp`

```cpp
static int on_headers_complete(llhttp_t* parser) {
    // ...
    if (!self.current_.headers.get("upgrade").empty() || ...) {
        return 2;  // ← nombre magique
    }
    return HPE_OK;
}
```

La valeur `2` est une constante interne de llhttp qui signifie « pause et
signale upgrade ». Elle n'est pas définie comme constante nommée dans les
headers publics de llhttp. Un commentaire explicatif s'impose au minimum :
`return 2; // HPE_PAUSED_UPGRADE: signal llhttp to pause for upgrade`.

---

## 13. Pattern de file d'écriture dupliqué

Le pattern complet de gestion d'une file d'écriture asynchrone est
implémenté deux fois de façon quasi-identique :

- `server::impl::session` : `writes_`, `writing_`, `pending_write_bytes_`,
  `flush_next()`, `on_write()`
- `websocket::session::state` : `writes`, `writing`, `pending_write_bytes`,
  `flush_next()`, `on_write()`

Les deux implémentations gèrent le même problème (écriture séquentielle
sans retour synchrone, close-after, compteur de bytes en attente) avec la
même logique. Une abstraction commune (`async_write_queue` ou similaire)
éviterait cette duplication et centraliserait les corrections futures.

---

## 14. `(void)req` vs paramètre non nommé dans les exemples

Les exemples n'adoptent pas de convention uniforme pour ignorer un paramètre
inutilisé :

```cpp
// Avec (void)
srv.get("/health", [](uvp::http::request& req, uvp::http::response& res) {
    (void)req;
    res.text("ok");
});

// Sans nom
srv.get("/health", [](uvp::http::request&, uvp::http::response& res) {
    res.text("ok");
});
```

La forme sans nom est idiomatique en C++ moderne et plus concise. Le pattern
`(void)param` est utilisé dans certains fichiers, pas dans d'autres.

---

## 15. Sérialisation incorrecte du booléen dans `local_json_api.cpp`

**Fichier :** `examples/local_json_api.cpp`

```cpp
out << "\",\"done\":\"" << (value.done ? "true" : "false") << "\"";
```

Le champ `done` est sérialisé comme une chaîne JSON (`"true"` / `"false"`)
plutôt que comme un booléen JSON (`true` / `false`). La différence est
notable pour tout client qui parse le JSON de façon stricte.

---

## 16. `require_positive` non appelé pour `max_body_bytes`

**Fichier :** `src/http/server_options.cpp`

Les setters `max_header_bytes`, `max_pending_write_bytes`,
`max_pending_responses_per_connection` valident leur argument via
`require_positive`. Le setter `max_body_bytes` ne le fait pas :

```cpp
server_options& server_options::max_body_bytes(std::size_t value) & {
    max_body_bytes_ = value;    // pas de validation
    return *this;
}
```

Aucun commentaire n'explique si c'est intentionnel (0 signifiant « pas de
body ») ou un oubli. La divergence avec les autres setters nuit à la
lisibilité.

---

## Résumé priorisé

| Priorité | Fichier(s) | Problème |
|---|---|---|
| 🔴 Bug | `server.cpp` | Référence pendante après déplacement de `server` |
| 🔴 Correctness | `local_json_api.cpp` | Booléen sérialisé en string JSON |
| 🟠 Sécurité / maintenance | `websocket/session.cpp` | SHA-1 fait maison, non testé |
| 🟠 Performance | `websocket/session.cpp` | `read_buffer` : erase O(n) en tête |
| 🟠 Duplication | `response.cpp`, exemples | `escape_json` dupliqué 3× avec différences |
| 🟠 Duplication | `server.cpp`, `websocket/session.cpp` | File d'écriture dupliquée |
| 🟡 Lisibilité | `server.cpp` | `header_name_equals` alloue inutilement |
| 🟡 Lisibilité | `server.cpp` | `reason_phrase_for` duplique `reason_phrase` |
| 🟡 Lisibilité | `http1_state_machine.cpp` | Nombre magique `return 2` sans commentaire |
| 🟡 Cohérence | multiple | Convention d'underscore member incohérente |
| 🟡 Cohérence | `websocket/session.cpp` | Variable locale `on_write` occulte la méthode |
| 🟡 Cohérence | `server_options.cpp` | `max_body_bytes` sans validation contrairement aux autres |
| 🟡 Mémoire | `http1_state_machine.hpp` | `http1_event` porte toujours deux champs dont un vide |
| 🟢 Style | exemples | `(void)req` vs paramètre non nommé incohérent |
