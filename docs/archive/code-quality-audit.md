# Audit qualité de code — uvpp-protocols
_(Temporary working file - sorry for the French)_

Cet audit couvre l'écriture, la cohérence, la lisibilité et la correction du
code. Il est distinct de l'audit API archivé (`api-audit.md`) ; les deux se
complètent.

**Statut : clôturé.** Les correctifs nécessaires ont été appliqués, les sujets
volontairement différés sont documentés comme décisions ou proposals, et les
points purement stylistiques ont été normalisés ou classés.

---

## 1. Bug : référence pendante après déplacement de `server`

**Fichier :** `src/http/server.cpp`

**Résolu :** `uvp::http::server` est maintenant explicitement non déplaçable.

`server::impl` stocke une référence `server& owner` initialisée dans le
constructeur de `server` :

```cpp
server::server(uv::loop& loop, server_options options)
    : loop_(&loop), options_(options), impl_(std::make_unique<impl>(*this)) {
```

Le constructeur de déplacement était `= default`, ce qui déplaçait `impl_` sans
mettre à jour `impl_->owner`. Après un déplacement, `impl_->owner` référençait
l'ancien objet `server` (moved-from). Tous les accès ultérieurs à
`owner_.owner.router_`, `owner_.owner.options_`, etc. depuis les sessions
actives étaient un comportement indéfini.

Le déplacement a été supprimé plutôt que réparé, car un serveur actif possède
des listeners, sessions et callbacks asynchrones dont le contrat de déplacement
serait ambigu.

---

## 2. WebSocket : responsabilité explicite du pong

**Fichier :** `src/websocket/session.cpp`

Ce point a été corrigé avec `accept_options::auto_pong(bool)`.

```cpp
void dispatch_ping(std::span<const std::byte> payload) {
    auto handle = session{shared_from_this()};
    if (on_ping) {
        on_ping(handle, payload);  // user callback
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

**Statut : résolu.** Le calcul `Sec-WebSocket-Accept` a été extrait dans
`src/websocket/detail/handshake.cpp` et utilise OpenSSL `Crypto` via l'API EVP
haut niveau. OpenSSL reste un détail d'implémentation privé du module
WebSocket, compatible avec la modularisation future des targets. Le vecteur RFC
6455 (`dGhlIHNhbXBsZSBub25jZQ==` -> `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`) est testé
unitairement.

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

**Statut : résolu.** Le buffer entrant reste un `std::vector<std::byte>` pour
conserver un parsing contigu et cache-friendly, mais la session maintient
maintenant un `read_offset`. Les frames consommées avancent cet offset et le
buffer n'est compacté que lorsque tout est consommé ou lorsque le préfixe mort
devient significatif. Un test WebSocket local envoie deux frames client
coalescées dans un même write pour valider l'enchaînement.

---

## 5. JSON sérialisé par `uvp::json`

**Fichiers :** `src/http/response.cpp`, `examples/log_streaming.cpp`,
`examples/local_json_api.cpp`

Ce point a été corrigé : les helpers `escape_json` locaux ont été supprimés au
profit de `uvp::json` (`nlohmann::json`). `response::json(const uvp::json&)`
utilise `dump()`, et les exemples construisent de vraies valeurs JSON au lieu
de concaténer des chaînes.

---

## 6. 🟢 Décision — `as_bytes()` défini dans deux unités de traduction

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

Décision : ne pas introduire de header utilitaire commun pour ce micro-helper.
Les deux fonctions ont une liaison interne et ne créent pas de risque ODR. Le
helper local dans `server.cpp` garde lisible l'écriture du buffer possédé ; celui
de l'exemple reste cosmétique et acceptable.

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

**Décision : règle documentée, migration planifiée.** Les règles de stockage,
suffixe `_` et accès public sont maintenant décrites dans
[`api-principles.md`](../design/api-principles.md). La remise à plat du code
est suivie dans
[`member-naming-normalization.md`](../proposals/member-naming-normalization.md)
afin de traiter ce changement comme une passe mécanique dédiée plutôt qu'un
refactoring opportuniste.

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

**Statut : résolu.** La variable locale a été renommée `write_callback`, ce qui
distingue clairement le callback applicatif déplacé depuis la file d'écriture
de la méthode `on_write(...)`.

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

**Statut : résolu.** `headers::names_equal` est maintenant une méthode statique
publique et `server.cpp` l'utilise directement :

```cpp
bool header_name_equals(std::string_view name, std::string_view expected) noexcept {
    return headers::names_equal(name, expected);
}
```

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

**Statut : résolu.** Le helper local ne duplique plus le `switch` et délègue
directement à `reason_phrase(status)` :

```cpp
std::string_view reason_phrase_for(unsigned int status_code) noexcept {
    return reason_phrase(static_cast<status>(status_code));
}
```

Les codes numériques valides qui ne correspondent à aucun énumérateur nommé
restent acceptés par `response::status(unsigned int)` et sérialisés avec une
reason phrase vide.

---

## 11. ✅ Résolu — `http1_event` transporte toujours les deux champs

**Fichier :** `src/http/detail/http1_state_machine.hpp`

Avant correction, `http1_event` portait toujours un `http1_message` et une
`std::string` de body :

```cpp
struct http1_event {
    type event_type = type::complete;
    http1_message message;   // vide pour les events body
    std::string body;        // vide pour les events headers/complete
};
```

Pour les événements de type `body`, `message` était un `http1_message`
default-construit. Pour les événements `headers` et `complete`, `body` était une
string vide. Chaque événement portait systématiquement des données inutiles.

Résolution : `http1_event` utilise maintenant un `std::variant` de payloads
typés (`headers_payload`, `body_payload`, `complete_payload`) et expose des
helpers d'accès alignés avec le type de l'événement.

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

**Statut : résolu.** Le `return 2` porte maintenant un commentaire local qui
explique le contrat llhttp : cette valeur met le parser en pause avec
`HPE_PAUSED_UPGRADE`.

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

**Décision : abstraction différée.** La duplication est réelle, mais les deux
files n'ont déjà plus la même politique :

- HTTP couple la file au pipelining, aux slots de réponse, au streaming
  chunked, aux upgrades et au comptage des réponses en vol ;
- WebSocket couple la file aux frames, aux callbacks d'écriture applicatifs,
  au close handshake et à l'adaptateur `byte_stream`.

Extraire maintenant un gestionnaire commun introduirait un vocabulaire
générique et des hooks sans réduire fortement la complexité. Les deux
cheminements restent donc séparés pour préserver la lisibilité locale.

Le sujet doit être réouvert si un troisième protocole réimplémente le même
noyau, ou si un correctif de file d'écriture doit être appliqué une seconde
fois. Dans ce cas, l'abstraction devra porter uniquement la mécanique FIFO
séquentielle (payload possédé, write actif, compteur de bytes), et laisser les
politiques HTTP/WebSocket aux sessions.

---

## 14. ✅ Résolu — `(void)req` vs paramètre non nommé dans les exemples

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

Résolution : les exemples et extraits de documentation concernés utilisent
maintenant des paramètres non nommés lorsque le paramètre n'est pas utilisé.
Les `(void)` restants ignorent soit une valeur de retour, soit des variables
locales volontairement montrées dans un extrait explicatif.

---

## 15. ✅ Résolu — Sérialisation incorrecte du booléen dans `local_json_api.cpp`

**Fichier :** `examples/local_json_api.cpp`

```cpp
out << "\",\"done\":\"" << (value.done ? "true" : "false") << "\"";
```

Le champ `done` est sérialisé comme une chaîne JSON (`"true"` / `"false"`)
plutôt que comme un booléen JSON (`true` / `false`). La différence est
notable pour tout client qui parse le JSON de façon stricte.

Résolution : l'exemple construit maintenant de vraies valeurs `uvp::json`.
`done` est sérialisé depuis un booléen C++ et sort donc comme booléen JSON.

---

## 16. ✅ Résolu — `require_positive` non appelé pour `max_body_bytes`

**Fichier :** `src/http/server_options.cpp`

Avant correction, les setters `max_header_bytes`, `max_pending_write_bytes`,
`max_pending_responses_per_connection` validaient leur argument via
`require_positive`, mais le setter `max_body_bytes` ne le faisait pas :

```cpp
server_options& server_options::max_body_bytes(std::size_t value) & {
    max_body_bytes_ = value;    // pas de validation
    return *this;
}
```

Aucun commentaire n'expliquait si c'était intentionnel (0 signifiant « pas de
body ») ou un oubli. La divergence avec les autres setters nuisait à la
lisibilité.

Résolution : `server_options::max_body_bytes(...)` et
`server_options::validate()` refusent maintenant `0`. La documentation précise
que `body::none{}` modélise une route sans body. La sentinelle `0` encore
utilisée en interne par `route_options` pour exprimer l'héritage de la limite
serveur est suivie dans la proposal
[`route-body-limit-inheritance`](../proposals/route-body-limit-inheritance.md).

---

## Résumé priorisé

| Priorité | Fichier(s) | Problème |
|---|---|---|
| ✅ Résolu | `server.cpp` | `server` non déplaçable, plus de référence pendante |
| ✅ Résolu | `websocket/detail/handshake.cpp` | SHA-1 WebSocket délégué à OpenSSL EVP et testé |
| ✅ Résolu | `websocket/session.cpp` | `read_buffer` utilise un offset et un compactage amorti |
| ✅ Résolu | exemples | JSON sérialisé par `uvp::json` |
| 🟢 Décision | `server.cpp`, `examples/log_streaming.cpp` | Helpers `as_bytes` locaux tolérés |
| 🟢 Décision | `server.cpp`, `websocket/session.cpp` | Files d'écriture séparées, abstraction différée |
| ✅ Résolu | `server.cpp`, `headers.hpp` | `header_name_equals` compare sans allocation |
| ✅ Résolu | `server.cpp` | `reason_phrase_for` délègue à `reason_phrase` |
| ✅ Résolu | `http1_state_machine.cpp` | `return 2` documente le signal `HPE_PAUSED_UPGRADE` |
| 🟢 Cadré | multiple | Convention d'underscore member documentée, migration proposée |
| ✅ Résolu | `websocket/session.cpp` | Variable locale `on_write` renommée `write_callback` |
| ✅ Résolu | `server_options.cpp` | `max_body_bytes` valide les limites serveur strictement positives |
| ✅ Résolu | `http1_state_machine.hpp` | `http1_event` porte un payload typé via `std::variant` |
| ✅ Résolu | exemples | Paramètres inutilisés non nommés |
| ✅ Résolu | `examples/local_json_api.cpp` | `done` sérialisé comme booléen JSON |
