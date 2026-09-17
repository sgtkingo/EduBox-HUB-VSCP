# VSCP Protocol

Aktuální API verze: `1.5`. Verze knihovny VSCP: `2.1.0`.

**VSCP** (*Virtual Sensors Communication Protocol*) je jednoduchý textový protokol pro výměnu zpráv mezi řídicí aplikací a cílovým zařízením. Protokol je navržený pro scénáře, kde je potřeba číst hodnoty senzorů, nastavovat akční členy, konfigurovat zařízení a potvrzovat připojení k fyzickým nebo logickým pinům.

VSCP používá model **request-response**: jedna strana odešle jeden příkaz a druhá strana odpoví jednou zprávou. Všechny příkazy používají stejný textový formát.

## 1. Základní vlastnosti

- textový protokol čitelný člověkem,
- synchronní model `request -> response`,
- zprávy ve formátu podobném URL query stringu,
- všechny hodnoty jsou přenášeny jako textové řetězce,
- každá odpověď obsahuje stav `status`,
- příkazy rozlišují inicializaci, připojení, čtení, konfiguraci, řízení a reset.

Protokol může být přenášen přes libovolný transport, který zachová řádkově oddělené textové zprávy, typicky například UART, USB serial, TCP socket nebo jiný streamový kanál.

## 2. Transportní vrstva

VSCP nedefinuje konkrétní fyzickou ani linkovou vrstvu. Předpokládá pouze, že jedna zpráva je přenesena jako samostatný textový řádek.

Doporučené obecné požadavky:

| Vlastnost | Doporučení |
| --- | --- |
| Kódování | ASCII nebo UTF-8 bez řídicích znaků v hodnotách |
| Ukončení zprávy | `\n` |
| Model komunikace | synchronní request-response |
| Paralelní requesty | nedoporučené bez dodatečného `sequence id` |
| Timeout | implementačně definovaný |

Příklad jedné zprávy:

```text
?type=UPDATE&id=temp_sensor
```

## 3. Wire format

VSCP zpráva má formát URL-like query stringu:

```text
?key=value&key2=value2
```

Pravidla:

- zpráva začíná znakem `?`,
- parametry jsou oddělené znakem `&`,
- klíč a hodnota jsou oddělené prvním znakem `=`,
- klíče by neměly být prázdné,
- hodnoty jsou přenášeny jako string,
- názvy klíčů jsou doporučeně case-sensitive,
- hodnoty obsahující `&`, `=`, mezery nebo speciální znaky by měly být URL-encodované, pokud to implementace podporuje,
- bez URL encodingu je bezpečné používat pouze jednoduché alfanumerické hodnoty.

Příklad:

```text
?type=CONTROL&id=led_01&brightness=80
```

## 4. Stav odpovědi

Každá response obsahuje parametr `status`.

| Hodnota | Význam |
| --- | --- |
| `status=1` | příkaz byl úspěšně přijat a proveden |
| `status=0` | příkaz selhal |

Při chybě se doporučuje doplnit parametr `error`.

Úspěšná odpověď:

```text
?id=temp_sensor&status=1&temperature=24.52
```

Chybná odpověď:

```text
?id=temp_sensor&status=0&error=Device not found
```

U příkazů pracujících s konkrétním zařízením se doporučuje, aby response obsahovala stejné `id` jako request. Výjimkou může být `INIT`, protože inicializuje samotný protokol, ne konkrétní zařízení.

## 5. Obecný device model

VSCP pracuje s obecnou představou zařízení identifikovaného pomocí `id` nebo `uid`. Protokol sám nevynucuje datové typy, rozsahy ani validační pravidla. Ty jsou odpovědností aplikační vrstvy, katalogu zařízení nebo konkrétní implementace.

Zařízení může obsahovat tři typické skupiny dat:

| Skupina | Význam | Typický příkaz |
| --- | --- | --- |
| read values | hodnoty čtené ze zařízení, například teplota nebo vlhkost | `UPDATE` |
| write/control values | hodnoty zapisované do zařízení, například jas, otáčky, setpoint | `CONTROL` |
| config values | konfigurační parametry, například režim, citlivost, perioda měření | `CONFIG` |

Typické role zařízení:

| Role | Význam |
| --- | --- |
| `sensor` | poskytuje čtené hodnoty přes `UPDATE` |
| `actuator` | přijímá řídicí hodnoty přes `CONTROL` |
| `hybrid` | kombinuje čtení, řízení a konfiguraci |

## 6. Přehled příkazů

| Command | Request | Response | Účel |
| --- | --- | --- | --- |
| `INIT` | `?type=INIT&app=<name>&db=<version>&api=<version>` | `?status=1` | inicializace a ověření kompatibility protokolu |
| `CONNECT` | `?type=CONNECT&id=<uid>&pins=<csv>` | `?id=<uid>&status=1` | potvrzení připojení zařízení k pinům nebo kanálům |
| `DISCONNECT` | `?type=DISCONNECT&id=<uid>` | `?id=<uid>&status=1` | odpojení zařízení |
| `UPDATE` | `?type=UPDATE&id=<uid>` | `?id=<uid>&status=1&key=value...` | načtení aktuálních hodnot ze zařízení |
| `CONFIG` | `?type=CONFIG&id=<uid>&key=value...` | `?id=<uid>&status=1` | zápis konfiguračních parametrů |
| `CONTROL` | `?type=CONTROL&id=<uid>&key=value...` | `?id=<uid>&status=1` | zápis runtime řídicích hodnot |
| `RESET` | `?type=RESET&id=<uid>` | `?id=<uid>&status=1` | reset zařízení nebo jeho runtime stavu |
| `PING` | `?type=PING&side=<client/server>&seq=<number>` | `?type=PING&side=<server/client>&seq=<number>&status=1` | kontrola dostupnosti protistrany v obou směrech |

## 7. INIT

`INIT` navazuje nebo ověřuje protokolové spojení. Může sloužit ke kontrole verze API, verze katalogu zařízení nebo aplikačního profilu.

Request:

```text
?type=INIT&app=board&db=1.0&api=1.3
```

Parametry:

| Parametr | Povinný | Popis |
| --- | --- | --- |
| `type=INIT` | ano | typ příkazu |
| `app` | volitelné | název aplikace, profilu nebo katalogu |
| `db` | volitelné | verze katalogu zařízení nebo datového modelu |
| `api` | doporučené | verze VSCP API |

Úspěšná response:

```text
?status=1
```

Chybná response:

```text
?status=0&error=API mismatch
```

## 8. CONNECT

`CONNECT` potvrzuje, že dané zařízení má být spojeno s konkrétními fyzickými piny, logickými kanály nebo jinými vstupně-výstupními body.

Request:

```text
?type=CONNECT&id=heater_01&pins=3,5,6
```

Parametry:

| Parametr | Povinný | Popis |
| --- | --- | --- |
| `type=CONNECT` | ano | typ příkazu |
| `id` | ano | identifikátor zařízení |
| `pins` | podle profilu | čárkou oddělený seznam pinů nebo kanálů |

Úspěšná response:

```text
?id=heater_01&status=1
```

Chybná response:

```text
?id=heater_01&status=0&error=Pin conflict
```

## 9. DISCONNECT

`DISCONNECT` odpojí zařízení od aktuálního mapování pinů, kanálů nebo runtime spojení.

Request:

```text
?type=DISCONNECT&id=heater_01
```

Úspěšná response:

```text
?id=heater_01&status=1
```

Chybná response:

```text
?id=heater_01&status=0&error=Device not connected
```

## 10. UPDATE

`UPDATE` čte aktuální runtime hodnoty ze zařízení. Typicky se používá pro senzory nebo čitelné hodnoty hybridních zařízení.

Request:

```text
?type=UPDATE&id=temp_sensor
```

Úspěšná response:

```text
?id=temp_sensor&status=1&temperature=24.52
```

Response může obsahovat více hodnot:

```text
?id=env_sensor&status=1&temperature=24.52&humidity=41&pressure=1012
```

Doporučení:

- `UPDATE` by měl vracet pouze hodnoty určené ke čtení,
- řídicí nebo zapisovatelné hodnoty by se měly nastavovat přes `CONTROL`,
- neznámé nebo nedostupné hodnoty je vhodné vynechat nebo vrátit `status=0` s popisem chyby.

## 11. CONFIG

`CONFIG` zapisuje konfigurační hodnoty zařízení. Jde o parametry, které určují režim chování zařízení, ale nejsou přímým runtime výstupem.

Request:

```text
?type=CONFIG&id=temp_sensor&period=1000&unit=C
```

Úspěšná response:

```text
?id=temp_sensor&status=1
```

Chybná response:

```text
?id=temp_sensor&status=0&error=Invalid config value
```

Typické použití:

- perioda měření,
- měřicí rozsah,
- jednotky,
- mód zařízení,
- filtrace nebo citlivost měření.

## 12. CONTROL

`CONTROL` zapisuje runtime řídicí hodnoty. Používá se pro akční členy nebo pro zapisovatelné hodnoty hybridních zařízení.

Request:

```text
?type=CONTROL&id=led_01&brightness=80
```

Úspěšná response:

```text
?id=led_01&status=1
```

Příklad zápisu cílové hodnoty regulátoru:

```text
?type=CONTROL&id=heater_01&set_point=32
?id=heater_01&status=1
```

Chybný příklad zápisu read-only hodnoty:

```text
?type=CONTROL&id=temp_sensor&temperature=50
?id=temp_sensor&status=0&error=Value is not writable
```

Doporučení:

- `CONTROL` by měl přijímat pouze zapisovatelné hodnoty,
- validační pravidla určuje aplikační profil nebo device model,
- response obvykle nemusí vracet novou hodnotu; stačí acknowledgement přes `status=1`.

## 13. RESET

`RESET` obnoví stav zařízení, jeho runtime hodnoty nebo jeho připojení podle pravidel konkrétní implementace.

Request:

```text
?type=RESET&id=heater_01
```

Úspěšná response:

```text
?id=heater_01&status=1
```

Chybná response:

```text
?id=heater_01&status=0&error=Reset failed
```

Volitelně může profil definovat speciální identifikátor, například:

```text
?type=RESET&id=all
```

Takové rozšíření by mělo být explicitně popsáno v aplikačním profilu.

## 14. Typické komunikační scénáře

### 14.1 Sensor example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=temp_sensor&pins=1
Device -> Controller: ?id=temp_sensor&status=1

Controller -> Device: ?type=UPDATE&id=temp_sensor
Device -> Controller: ?id=temp_sensor&status=1&temperature=24.52

Controller -> Device: ?type=DISCONNECT&id=temp_sensor
Device -> Controller: ?id=temp_sensor&status=1
```

### 14.2 Actuator example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=led_01&pins=3
Device -> Controller: ?id=led_01&status=1

Controller -> Device: ?type=CONFIG&id=led_01&enabled=1
Device -> Controller: ?id=led_01&status=1

Controller -> Device: ?type=CONTROL&id=led_01&brightness=80
Device -> Controller: ?id=led_01&status=1
```

### 14.3 Hybrid regulator example

```text
Controller -> Device: ?type=INIT&app=board&db=1.0&api=1.3
Device -> Controller: ?status=1

Controller -> Device: ?type=CONNECT&id=heater_01&pins=3,5,6
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=CONFIG&id=heater_01&speed=4
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=CONTROL&id=heater_01&set_point=32
Device -> Controller: ?id=heater_01&status=1

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=24

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=28

Controller -> Device: ?type=UPDATE&id=heater_01
Device -> Controller: ?id=heater_01&status=1&temperature=32
```

## 15. Error handling

Běžné chybové stavy:

- neznámý `type`,
- chybějící `id`,
- neznámé zařízení,
- neplatný pin nebo konflikt pinů,
- neplatná hodnota,
- zápis do read-only hodnoty,
- čtení nedostupné hodnoty,
- nekompatibilní API verze,
- timeout nebo neúplná response.

Doporučený formát chyby:

```text
?id=<uid>&status=0&error=<human_readable_message>
```

Příklad:

```text
?id=led_01&status=0&error=Brightness out of range
```

## 16. Doporučená validační pravidla

Implementace by měla kontrolovat zejména:

- že request začíná znakem `?`,
- že obsahuje `type`,
- že příkazy pracující se zařízením obsahují `id`,
- že response obsahuje `status`,
- že response `id` odpovídá requestu, pokud se příkaz týká konkrétního zařízení,
- že `CONTROL` nezapisuje read-only hodnoty,
- že `CONFIG` pracuje pouze s konfiguračními parametry,
- že `UPDATE` nevrací interní nebo zapisovatelné hodnoty, pokud to profil výslovně nepovoluje.

## 17. Omezení základního VSCP profilu

Základní profil VSCP je jednoduchý a záměrně minimální. Proto má několik omezení:

- bez URL encodingu nejsou bezpečné hodnoty obsahující `&`, `=` nebo mezery,
- protokol sám nedefinuje checksum,
- protokol sám nedefinuje šifrování ani autentizaci,
- protokol sám nedefinuje `sequence id`,
- paralelní requesty nad jedním streamem nejsou bezpečné bez rozšíření,
- datové typy a rozsahy hodnot jsou mimo základní wire format,
- konkrétní význam pinů a device modelu musí definovat aplikační profil.

## 18. Minimální kompatibilní implementace

Minimální implementace VSCP by měla podporovat:

1. parsování query-string zprávy začínající `?`,
2. čtení parametru `type`,
3. generování response se `status=1` nebo `status=0`,
4. podporu `INIT`,
5. alespoň jeden device command, například `UPDATE` pro senzor nebo `CONTROL` pro akční člen,
6. chybové odpovědi s parametrem `error`.

Minimální příklad:

```text
?type=INIT&api=1.3
?status=1

?type=UPDATE&id=temp_sensor
?id=temp_sensor&status=1&temperature=24.52
```

## 19. Obousměrný PING

`PING` ověřuje dostupnost protistrany. Funguje před `INIT`, nevyžaduje `id`
a nemění inicializaci protokolu ani připojení zařízení. Role odesílatele je
pevná: Panel/klient používá `side=client`, Board/server používá `side=server`.
Obě strany smějí zahájit ping, i současně.

```text
Panel -> Board: ?type=PING&side=client&seq=42
Board -> Panel: ?type=PING&side=server&seq=42&status=1

Board -> Panel: ?type=PING&side=server&seq=17
Panel -> Board: ?type=PING&side=client&seq=17&status=1
```

Požadavek neobsahuje `status`. Platný požadavek dostane odpověď s `type=PING`,
vlastním `side`, převzatým `seq` a `status=1`. Na zprávu obsahující `status`
se znovu neodpovídá. Pořadí parametrů není významné.

`seq` je desítkové celé číslo 1–4294967295 bez úvodních nul. Každý endpoint
má vlastní čítač, který se zvyšuje pro každý pokus a po maximu pokračuje od 1.
Čekající ping přijme pouze `status=1` od opačné role se shodným `seq`, před
vypršením timeoutu. Nesprávné role/sekvence, neplatné pingy, duplicitní,
nevyžádané a opožděné odpovědi se ignorují. Párování platí v rámci životnosti
endpointu; sekvence nejsou identifikátorem relace napříč restartem.

Server má nejvýše jeden vlastní čekající ping na transport; více transportů
je nezávislých. Příchozí pingy se obsluhují i během čekání na odpověď na běžný
příkaz. Díky `type=PING` nemůže potvrzení pingu nahradit odpověď na `UPDATE`
nebo jinou transakci. Ostatní příkazy zachovávají původní formát.

Knihovna nabízí synchronní `Client::ping()`, idle obsluhu `Client::poll()`,
neblokující `Server::ping(transport, timeoutMs)` a `Server::pingResult(transport)`.
Serverův výsledek obsahuje sekvenci a stav `Idle`, `Pending`, `Ok`, `Timeout`
nebo `WriteError`. Čtení transportu musí mít jediného vlastníka; metody se
volají ze stejného kontextu. Polling vyžaduje neblokující transport.

Interval pingů a reakce na výpadek jsou aplikační politika. Úspěšný ping
potvrzuje komunikaci, nikoli funkčnost senzoru, platnost `INIT` nebo připojení
pinů. Knihovna nezapíná periodické pingy ani automatické obnovení spojení.
