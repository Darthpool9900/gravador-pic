// Revisao de controle de alimentacao e identificacao - 22/10/2026.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

constexpr uint8_t VPP = 25;
constexpr uint8_t VDD = 26;
constexpr uint8_t PGC = 27;
constexpr uint8_t PGD_OUT = 32;
constexpr uint8_t PGD_IN = 33;

constexpr uint16_t PALAVRAS = 8192;
constexpr size_t MAX_ARQUIVO = 60000;
constexpr unsigned TEMPO_US = 50;

// Esperas conservadoras de bancada; nao sao minimos do protocolo.
constexpr unsigned DESCARGA_MS = 3000;
constexpr unsigned ESTABILIZAR_VDD_MS = 30;
constexpr unsigned ESTABILIZAR_VPP_MS = 10;

WebServer server(80);

uint16_t memoria[PALAVRAS];
uint8_t recebido[PALAVRAS];
uint16_t configuracao;
uint8_t configRecebida;
uint16_t ultimoEndereco;

String arquivo;
String mensagem = "Aguardando arquivo HEX.";
String erroUpload;

bool ocupado = false;
bool iniciar = false;
bool uploadAceito = false;
bool uploadCompleto = false;

const char PAGINA[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gravador PIC</title>
<style>
:root{color-scheme:dark}
body{font-family:Arial;background:#111;color:#eee;
max-width:760px;margin:30px auto;padding:20px}
section{background:#202020;padding:24px;border-radius:12px}
button,input{font-size:16px;margin:12px 0}
button{background:#287b45;color:white;border:0;
padding:13px 20px;border-radius:7px;cursor:pointer}
button:disabled{opacity:.5}
pre{background:#080808;padding:16px;white-space:pre-wrap;
overflow-wrap:anywhere;border-radius:8px}
</style>
</head>
<body>
<section>
<h1>Gravador PIC por Wi-Fi</h1>
<p>PIC16F877A — arquivo Intel HEX.</p>
<p>A gravacao apaga o conteudo anterior do PIC.</p>
<form id="form">
<input id="hex" type="file" accept=".hex" required><br>
<button id="botao">Enviar, apagar e gravar</button>
</form>
<pre id="status">Consultando...</pre>
</section>
<script>
const status=document.getElementById('status');
const botao=document.getElementById('botao');

async function atualizar(){
  try{
    const r=await fetch('/status',{cache:'no-store'});
    if(r.ok) status.textContent=await r.text();
  }catch(e){}
}

document.getElementById('form').onsubmit=async e=>{
  e.preventDefault();
  const f=document.getElementById('hex').files[0];
  if(!f)return;
  botao.disabled=true;
  status.textContent='Enviando arquivo...';
  const dados=new FormData();
  dados.append('hex',f);
  try{
    const r=await fetch('/gravar',{method:'POST',body:dados});
    status.textContent=await r.text();
  }catch(e){
    status.textContent='Falha de comunicacao. Consulte o estado antes de repetir.';
  }finally{
    botao.disabled=false;
  }
};
setInterval(atualizar,1000);
atualizar();
</script>
</body>
</html>
)HTML";

void informar(const String &texto) {
  mensagem = texto;
  Serial.println(texto);
}

void pgc(bool alto) {
  digitalWrite(PGC, alto ? LOW : HIGH);
}

void pgd(bool alto) {
  digitalWrite(PGD_OUT, alto ? LOW : HIGH);
}

void enviarBit(bool valor) {
  pgd(valor);
  delayMicroseconds(TEMPO_US);
  pgc(true);
  delayMicroseconds(TEMPO_US);
  pgc(false);
  delayMicroseconds(TEMPO_US);
}

void comando(uint8_t valor) {
  for (uint8_t i = 0; i < 6; i++) {
    enviarBit((valor >> i) & 1);
  }
  pgd(true);
  delayMicroseconds(TEMPO_US);
}

void enviarPalavra(uint16_t valor) {
  enviarBit(false);
  for (uint8_t i = 0; i < 14; i++) {
    enviarBit((valor >> i) & 1);
  }
  enviarBit(false);
  pgd(true);
  delayMicroseconds(TEMPO_US);
}

uint16_t lerAtual() {
  comando(0x04);
  uint16_t valor = 0;

  for (uint8_t i = 0; i < 16; i++) {
    pgc(true);
    delayMicroseconds(TEMPO_US);

    if (i >= 1 && i <= 14 && digitalRead(PGD_IN)) {
      valor |= uint16_t(1) << (i - 1);
    }

    pgc(false);
    delayMicroseconds(TEMPO_US);
  }
  return valor & 0x3FFF;
}

// Os transistores de PGC/PGD invertem: GPIO HIGH mantem o PIC em LOW.
// Desligar VPP primeiro; manter os dois sinais baixos antes de remover VDD.
void desligarAlvo() {
  digitalWrite(VPP, LOW);
  delay(20);
  pgc(false);
  pgd(false);
  digitalWrite(VDD, LOW);
}

void entrar() {
  desligarAlvo();
  delay(DESCARGA_MS);
  digitalWrite(VDD, HIGH);
  delay(ESTABILIZAR_VDD_MS);
  digitalWrite(VPP, HIGH);
  delay(ESTABILIZAR_VPP_MS);
}

void sair() {
  desligarAlvo();
  delay(100);
  // A proxima entrada sempre executa a espera completa de descarga.
}

void selecionarConfig(uint8_t deslocamento) {
  comando(0x00);
  enviarPalavra(0x3FFF);

  for (uint8_t i = 0; i < deslocamento; i++) {
    comando(0x06);
  }
}

void gravarPalavra(uint16_t valor) {
  comando(0x17);
  comando(0x02);
  enviarPalavra(valor);
  comando(0x18);
  delayMicroseconds(1000);
  comando(0x17);
  delayMicroseconds(100);
}

void atender() {
  server.handleClient();
  delay(1);
}

String hexadecimal(uint16_t valor) {
  char texto[7];
  snprintf(texto, sizeof(texto), "0x%04X", unsigned(valor));
  return String(texto);
}

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool analisarHex(String &erro) {
  memset(memoria, 0, sizeof(memoria));
  memset(recebido, 0, sizeof(recebido));

  configuracao = 0;
  configRecebida = 0;
  ultimoEndereco = 0;

  bool temPrograma = false;
  bool fim = false;
  uint32_t base = 0;
  size_t pos = 0;
  unsigned linha = 0;

  while (pos < arquivo.length()) {
    size_t inicio = pos;

    while (pos < arquivo.length() &&
           arquivo[pos] != '\n' && arquivo[pos] != '\r') {
      pos++;
    }

    String registro = arquivo.substring(inicio, pos);
    registro.trim();

    while (pos < arquivo.length() &&
           (arquivo[pos] == '\n' || arquivo[pos] == '\r')) {
      pos++;
    }

    if (registro.isEmpty()) continue;
    linha++;

    if (fim) {
      erro = "Dados depois do fim do arquivo.";
      return false;
    }

    if (registro[0] != ':' ||
        registro.length() < 11 ||
        ((registro.length() - 1) % 2) != 0) {
      erro = "Registro invalido na linha " + String(linha);
      return false;
    }

    size_t quantidade = (registro.length() - 1) / 2;
    if (quantidade > 260) {
      erro = "Registro muito longo.";
      return false;
    }

    uint8_t bytes[260];
    uint8_t soma = 0;

    for (size_t i = 0; i < quantidade; i++) {
      int a = nibble(registro[1 + i * 2]);
      int b = nibble(registro[2 + i * 2]);

      if (a < 0 || b < 0) {
        erro = "Caractere invalido na linha " + String(linha);
        return false;
      }

      bytes[i] = (a << 4) | b;
      soma += bytes[i];
    }

    uint8_t tamanho = bytes[0];
    uint16_t offset = (uint16_t(bytes[1]) << 8) | bytes[2];
    uint8_t tipo = bytes[3];

    if (quantidade != size_t(tamanho) + 5 || soma != 0) {
      erro = "Comprimento ou checksum invalido na linha " + String(linha);
      return false;
    }

    if (tipo == 1) {
      if (tamanho != 0 || offset != 0) {
        erro = "Registro de fim invalido.";
        return false;
      }
      fim = true;
      continue;
    }

    if (tipo == 2 || tipo == 4) {
      if (tamanho != 2 || offset != 0) {
        erro = "Endereco estendido invalido.";
        return false;
      }

      uint32_t valor = (uint16_t(bytes[4]) << 8) | bytes[5];
      base = valor << (tipo == 2 ? 4 : 16);
      continue;
    }

    if (tipo != 0) {
      erro = "Tipo de registro nao suportado: " + String(tipo);
      return false;
    }

    if (uint32_t(offset) + tamanho > 0x10000UL) {
      erro = "Registro ultrapassa o segmento.";
      return false;
    }

    for (uint16_t i = 0; i < tamanho; i++) {
      uint32_t endereco = base + offset + i;
      uint8_t dado = bytes[4 + i];
      uint8_t metade = (endereco & 1) ? 2 : 1;
      unsigned deslocamento = (endereco & 1) ? 8 : 0;

      if ((endereco & 1) && dado > 0x3F) {
        erro = "Palavra excede 14 bits.";
        return false;
      }

      uint16_t *destino;
      uint8_t *mascara;

      if (endereco < 0x4000) {
        uint16_t indice = endereco / 2;
        destino = &memoria[indice];
        mascara = &recebido[indice];
        temPrograma = true;
        if (indice > ultimoEndereco) ultimoEndereco = indice;
      } else if (endereco == 0x400E || endereco == 0x400F) {
        destino = &configuracao;
        mascara = &configRecebida;
      } else {
        erro = "Endereco nao suportado nesta versao: " +
               String(endereco, HEX);
        return false;
      }

      uint8_t anterior = (*destino >> deslocamento) & 0xFF;
      if ((*mascara & metade) && anterior != dado) {
        erro = "Dados conflitantes no HEX.";
        return false;
      }

      uint16_t manter = deslocamento ? 0x00FF : 0xFF00;
      *destino = (*destino & manter) | (uint16_t(dado) << deslocamento);
      *mascara |= metade;
    }
  }

  if (!fim || !temPrograma || configRecebida != 3) {
    erro = "HEX precisa conter programa, configuracao e registro de fim.";
    return false;
  }

  for (uint16_t i = 0; i <= ultimoEndereco; i++) {
    if (recebido[i] != 0 && recebido[i] != 3) {
      erro = "Palavra incompleta em " + hexadecimal(i);
      return false;
    }
  }

  // Este teste exige protecao desligada para verificar o programa.
  if (!(configuracao & 0x2000) ||
      (configuracao & 0x1030) != 0x1030) {
    erro = "Configuracao com protecao ativa ou bits reservados invalidos.";
    return false;
  }

  return true;
}

bool verificarPrograma(String &erro) {
  entrar();

  for (uint16_t i = 0; i <= ultimoEndereco; i++) {
    uint16_t esperado = recebido[i] ? memoria[i] : 0x3FFF;
    uint16_t lido = lerAtual();

    if (lido != esperado) {
      erro = "Endereco " + hexadecimal(i) +
             " | esperado " + hexadecimal(esperado) +
             " | lido " + hexadecimal(lido);
      sair();
      return false;
    }

    if ((i % 16) == 0 || i == ultimoEndereco) {
      informar("Verificando programa: " + String(i + 1) +
               "/" + String(ultimoEndereco + 1));
      atender();
    }

    if (i < ultimoEndereco) comando(0x06);
  }

  sair();
  return true;
}

bool programar(String &erro) {
  uint16_t id = 0;
  bool identificado = false;

  // Repetir apenas identificacao, sem repetir apagamento/escrita automaticamente.
  for (uint8_t tentativa = 1; tentativa <= 3; ++tentativa) {
    informar("Preparando o PIC e aguardando descarga. Identificacao " +
             String(tentativa) + "/3...");
    atender();
    entrar();
    selecionarConfig(6);
    id = lerAtual();
    sair();
    informar("ID: " + hexadecimal(id));
    if ((id & 0x3FF0) == 0x0E20) {
      identificado = true;
      break;
    }
    atender();
  }

  if (!identificado) {
    erro = "PIC16F877A nao identificado apos 3 tentativas. "
           "Nenhum apagamento executado. Confira VDD/VPP e conexoes com tudo desligado.";
    return false;
  }

  atender();
  informar("Apagando o PIC...");
  entrar();
  comando(0x1F);
  delay(20);
  sair();

  informar("Gravando programa...");
  entrar();

  for (uint16_t i = 0; i <= ultimoEndereco; i++) {
    if (recebido[i] && memoria[i] != 0x3FFF) {
      gravarPalavra(memoria[i]);
    }

    if ((i % 16) == 0 || i == ultimoEndereco) {
      informar("Gravando programa: " + String(i + 1) +
               "/" + String(ultimoEndereco + 1));
      atender();
    }

    if (i < ultimoEndereco) comando(0x06);
  }

  sair();

  if (!verificarPrograma(erro)) return false;

  informar("Gravando configuracao " + hexadecimal(configuracao));
  entrar();
  selecionarConfig(7);

  // Sequencia que passou no teste fisico: DS39589A.
  comando(0x02);
  enviarPalavra(configuracao);
  comando(0x08);
  delay(8);

  uint16_t imediata = lerAtual();
  sair();

  entrar();
  selecionarConfig(7);
  uint16_t persistente = lerAtual();
  sair();

  Serial.println("Config imediata: " + hexadecimal(imediata));
  Serial.println("Config nova entrada: " + hexadecimal(persistente));

  if (imediata != configuracao || persistente != configuracao) {
    erro = "Configuracao esperada " + hexadecimal(configuracao) +
           " | imediata " + hexadecimal(imediata) +
           " | nova entrada " + hexadecimal(persistente);
    return false;
  }

  // Confere se a gravacao da configuracao preservou o programa.
  if (!verificarPrograma(erro)) return false;

  informar("SUCESSO: programa e configuracao " +
           hexadecimal(configuracao) +
           " gravados e verificados.\n"
           "VPP e VDD comandados desligados. Desligue todas as alimentacoes antes de mover o PIC.");

  return true;
}

void receberArquivo() {
  HTTPUpload &u = server.upload();

  if (u.status == UPLOAD_FILE_START) {
    uploadAceito = !ocupado;
    if (!uploadAceito) return;

    arquivo = "";
    erroUpload = "";
    uploadCompleto = false;
    informar("Recebendo arquivo...");
  }

  if (!uploadAceito) return;

  if (u.status == UPLOAD_FILE_WRITE) {
    if (!erroUpload.isEmpty()) return;

    if (arquivo.length() + u.currentSize > MAX_ARQUIVO) {
      erroUpload = "Arquivo maior que 60000 bytes.";
      return;
    }

    if (!arquivo.concat(reinterpret_cast<const char *>(u.buf),
                        unsigned(u.currentSize))) {
      erroUpload = "Memoria insuficiente para receber o arquivo.";
    }
  } else if (u.status == UPLOAD_FILE_END) {
    uploadCompleto = true;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    erroUpload = "Envio interrompido.";
    uploadCompleto = false;
  }
}

void pedidoGravacao() {
  if (ocupado) {
    server.send(409, "text/plain; charset=utf-8",
                "Gravacao em andamento. Aguarde.");
    return;
  }

  String erro;

  if (!erroUpload.isEmpty()) {
    erro = erroUpload;
  } else if (!uploadCompleto || arquivo.isEmpty()) {
    erro = "Arquivo nao recebido completamente.";
  } else if (!analisarHex(erro)) {
    // O parser preenche a mensagem.
  } else {
    ocupado = true;
    iniciar = true;
    arquivo = "";

    informar("HEX validado. Gravacao agendada.");
    server.send(202, "text/plain; charset=utf-8", mensagem);
    return;
  }

  informar("HEX rejeitado: " + erro);
  server.send(400, "text/plain; charset=utf-8", mensagem);
}

void setup() {
  digitalWrite(VPP, LOW);
  digitalWrite(VDD, LOW);
  digitalWrite(PGC, HIGH);
  digitalWrite(PGD_OUT, HIGH);

  pinMode(VPP, OUTPUT);
  pinMode(VDD, OUTPUT);
  pinMode(PGC, OUTPUT);
  pinMode(PGD_OUT, OUTPUT);
  pinMode(PGD_IN, INPUT);

  Serial.begin(115200);
  desligarAlvo();
  delay(DESCARGA_MS);

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP("ESP32-TESTE-TCC", "teste1234")) {
    informar("Falha ao iniciar a rede Wi-Fi.");
    return;
  }

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", PAGINA);
  });

  server.on("/status", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", mensagem);
  });

  server.on("/gravar", HTTP_POST, pedidoGravacao, receberArquivo);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Pagina nao encontrada.");
  });

  server.begin();

  Serial.println("Rede: ESP32-TESTE-TCC");
  Serial.println("Senha: teste1234");
  Serial.print("Endereco: http://");
  Serial.println(WiFi.softAPIP());

  informar("Gravador revisao 22/10: aguardando HEX. PIC com VDD/VPP comandados desligados.");
}

void loop() {
  server.handleClient();

  if (iniciar) {
    iniciar = false;
    String erro;

    if (!programar(erro)) {
      sair();
      informar("FALHA: " + erro);
    }

    ocupado = false;
  }

  delay(1);
}