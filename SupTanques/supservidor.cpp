#include <iostream>     /* cerr */
#include <algorithm>
#include "supservidor.h"

using namespace std;

/* ========================================
   CLASSE SUPSERVIDOR
   ======================================== */

/// Construtor
SupServidor::SupServidor()
  : Tanks()
  , server_on(false)
  , LU()
  , sock_server()
  , thr_server()
{
  // Inicializa a biblioteca de sockets
  mysocket_status iResult = mysocket::init();
  if (iResult != mysocket_status::SOCK_OK)
  {
    cerr <<  "Biblioteca mysocket nao pode ser inicializada";
    exit(-1);
  }
}

/// Destrutor
SupServidor::~SupServidor()
{
  // Deve parar a thread do servidor
  server_on = false;

  // Fecha todos os sockets dos clientes
  for (auto& U : LU) U.close();

  // Fecha o socket de conexoes
  sock_server.close();

  // Espera o fim da thread do servidor
    if (thr_server.joinable()) thr_server.join();
  thr_server = thread();


  // Encerra a biblioteca de sockets
  mysocket::end();
}

/// Liga o servidor
bool SupServidor::setServerOn()
{
  // Se jah estah ligado, nao faz nada
  if (server_on) return true;

  // Liga os tanques
  setTanksOn();

  // Indica que o servidor estah ligado a partir de agora
  server_on = true;

  try
  {
    // Coloca o socket de conexoes em escuta
    mysocket_status iResult = sock_server.listen(SUP_PORT);

    // Em caso de erro, gera excecao
    if (iResult != mysocket_status::SOCK_OK) throw 1;

    thr_server = thread( [this]() { this->thr_server_main(); } );

    if (!thr_server.joinable()) throw 2;
  }

  catch(int i)
  {
    cerr << "Erro " << i << " ao iniciar o servidor\n";

    // Deve parar a thread do servidor
    server_on = false;

    // Fecha o socket do servidor
    sock_server.close();

    return false;
  }

  // Tudo OK
  return true;
}

/// Desliga o servidor
void SupServidor::setServerOff()
{
  // Se jah estah desligado, nao faz nada
  if (!server_on) return;

  // Deve parar a thread do servidor
  server_on = false;

  // Fecha todos os sockets dos clientes
  for (auto& U : LU) U.close();

  //ALTERADO
  // Fecha o socket de conexoes
  sock_server.close();

  // Espera pelo fim da thread do servidor
  if (thr_server.joinable()) thr_server.join();

  // Faz o identificador da thread apontar para thread vazia
  thr_server = thread();

  // Desliga os tanques
  setTanksOff();
}

/// Leitura do estado dos tanques
void SupServidor::readStateFromSensors(SupState& S) const
{
  // Estados das valvulas: OPEN, CLOSED
  S.V1 = v1isOpen();
  S.V2 = v2isOpen();
  // Niveis dos tanques: 0 a 65535
  S.H1 = hTank1();
  S.H2 = hTank2();
  // Entrada da bomba: 0 a 65535
  S.PumpInput = pumpInput();
  // Vazao da bomba: 0 a 65535
  S.PumpFlow = pumpFlow();
  // Estah transbordando (true) ou nao (false)
  S.ovfl = isOverflowing();
}

/// Leitura e impressao em console do estado da planta
void SupServidor::readPrintState() const
{
  if (tanksOn())
  {
    SupState S;
    readStateFromSensors(S);
    S.print();
  }
  else
  {
    cout << "Tanques estao desligados!\n";
  }
}

/// Impressao em console dos usuarios do servidor
void SupServidor::printUsers() const
{
  for (const auto& U : LU)
  {
    cout << U.login << '\t'
         << "Admin=" << (U.isAdmin ? "SIM" : "NAO") << '\t'
         << "Conect=" << (U.isConnected() ? "SIM" : "NAO") << '\n';
  }
}

/// Adicionar um novo usuario
bool SupServidor::addUser(const string& Login, const string& Senha,
                             bool Admin)
{
  // Testa os dados do novo usuario
  if (Login.size()<6 || Login.size()>12) return false;
  if (Senha.size()<6 || Senha.size()>12) return false;

  // Testa se jah existe usuario com mesmo login
  auto itr = find(LU.begin(), LU.end(), Login);
  if (itr != LU.end()) return false;

  // Insere
  LU.push_back( User(Login,Senha,Admin) );

  // Insercao OK
  return true;
}

/// Remover um usuario
bool SupServidor::removeUser(const string& Login)
{
  // Testa se existe usuario com esse login
  auto itr = find(LU.begin(), LU.end(), Login);
  if (itr == LU.end()) return false;

  // Remove
  LU.erase(itr);

  // Remocao OK
  return true;
}

/// A thread que implementa o servidor.
/// Comunicacao com os clientes atraves dos sockets.
void SupServidor::thr_server_main(void)
{
    Tanks tanks;
    mysocket_queue queue;
    mysocket_status iResult;
    int16_t cmd;


    while (server_on)
    {
        try
        {
           // Encerra se o socket de conexoes estiver fechado
            if (!sock_server.accepting())
            {
                throw std::runtime_error("Socket de conex�es fechado");
            }

            // Limpa a fila de sockets
            queue.clear();
            // Inclui na fila o socket de conexoes
            queue.include(sock_server);

            // Inclui na fila todos os sockets dos clientes conectados

            for (auto &U : LU)
            {
                if (U.isConnected())
                {
                    queue.include(U.sock);
                }
            }
            //Aguarda 10 segundos até que chegue dado em algum socket
            iResult = queue.wait_read(SUP_TIMEOUT * 1000);

            //Volta ao início em caso de TIMEOUT
            if (iResult == mysocket_status::SOCK_TIMEOUT)
                continue;

            //Encerra o servidor em caso de erro no socket
            if (iResult == mysocket_status::SOCK_ERROR)
                throw std::runtime_error("Erro no socket ao aguardar atividade");

          // Houve atividade em algum socket da fila:
          //   Testa se houve atividade nos sockets dos clientes. Se sim:
          //   - Leh o comando
          //   - Executa a acao
          //   = Envia resposta


            for (auto it = LU.begin(); it != LU.end();)
            {
                auto &U = *it;

                if (queue.had_activity(U.sock))
                {
                    iResult = U.sock.read_int16(cmd, -1);

                    if (iResult == mysocket_status::SOCK_DISCONNECTED || iResult == mysocket_status::SOCK_ERROR)
                    {
                        U.sock.close();
                        continue;
                    }

                    switch (cmd)
                    {


                    case CMD_GET_DATA:
                    {
                        SupState state;
                        readStateFromSensors(state);
                        U.sock.write_int16(CMD_OK);
                        U.sock.write_bytes(reinterpret_cast<const mybyte *>(&state), sizeof(state));
                        break;
                    }

                    case CMD_SET_V1:
                    {
                        int16_t v1_state;
                        iResult = U.sock.read_int16(v1_state, -1);

                        if (iResult == mysocket_status::SOCK_OK && U.isAdmin)
                        {
                            setV1Open(v1_state != 0);
                            U.sock.write_int16(CMD_OK);
                            std::cout << "CMD_SET_V1 DE " << U.login << " [OK]\n";
                        }
                        else
                        {
                            U.sock.write_int16(CMD_ERROR);
                        }
                        break;
                    }

                    case CMD_SET_V2:
                    {
                        int16_t v2_state;
                        iResult = U.sock.read_int16(v2_state, -1);

                        if (iResult == mysocket_status::SOCK_OK && U.isAdmin)
                        {
                            setV2Open(v2_state != 0);
                            U.sock.write_int16(CMD_OK);
                            std::cout << "CMD_SET_V2 DE " << U.login << " [OK]\n";
                        }
                        else
                        {
                            U.sock.write_int16(CMD_ERROR);
                        }
                        break;
                    }

                    case CMD_SET_PUMP:
                    {
                        int16_t pump_input;
                        iResult = U.sock.read_int16(pump_input, -1);

                        if (iResult == mysocket_status::SOCK_OK && U.isAdmin)
                        {
                            tanks.setPumpInput(static_cast<uint16_t>(pump_input));
                            U.sock.write_int16(CMD_OK);
                            std::cout << "CMD_SET_PUMP DE " << U.login << " [OK]\n";
                        }
                        else
                        {
                            U.sock.write_int16(CMD_ERROR);
                        }
                        break;
                    }
                      case CMD_LOGOUT:
                        {
                            U.sock.write_int16(CMD_OK);
                            std::cout << "CMD_LOGOUT (" << U.login << ") [OK]\n"; // Log da ação
                            U.sock.close();
                            break;
                        }
                    default:
                        U.sock.write_int16(CMD_ERROR);
                        break;
                    }
                }

                ++it;
            }

          //   Depois, testa se houve atividade no socket de conexao. Se sim:
          //   - [OK] Estabelece nova conexao em socket temporario
          //   - [OK] Leh comando, login e senha
          //   - [OK] Testa usuario - ve na lista de usuarios se já existe. Vê se login e senha são compatíveis.
          //   - [OK] Se deu tudo certo, faz o socket temporario ser o novo socket
          //     do cliente e envia confirmacao
            if (server_on && sock_server.connected() && queue.had_activity(sock_server)) {
              tcp_mysocket new_sock; // Socket temporário para o clienteS

              try {
                //Nova conexão em socket temporário
                mysocket_status iResult = sock_server.accept(new_sock);
                if (iResult != mysocket_status::SOCK_OK) {
                  throw std::runtime_error("Erro ao aceitar nova conexão");
              }

              // Receber comando de login do novo cliente
              int16_t cmd;
              iResult = new_sock.read_int16(cmd, -1);
              if (iResult != mysocket_status::SOCK_OK || cmd != CMD_LOGIN) {
                  throw std::runtime_error("Comando inválido ou erro ao receber comando de login");
              }

              // Receber login e senha do cliente
              std::string login, password;
              iResult = new_sock.read_string(login, -1);
              if (iResult != mysocket_status::SOCK_OK) {
                  throw std::runtime_error("Erro ao receber login do cliente");
              }

              iResult = new_sock.read_string(password, -1);
              if (iResult != mysocket_status::SOCK_OK) {
                  throw std::runtime_error("Erro ao receber senha do cliente");
              }

              // Verificar se o usuário já existe
              auto it = std::find(LU.begin(), LU.end(), login);
              if (it != LU.end()) {
                  // Verifica a senha do usuário existente
                  if (it->password == password) {
                      // Login bem-sucedido - associa o novo socket ao usuário
                      it->sock = std::move(new_sock);
                      std::cout << "CMD_LOGIN (" << login << ") [OK]\n";
                       if (it->isAdmin) {
                        it->sock.write_int16(CMD_ADMIN_OK); // Confirmação de login como administrador
                      } else {
                      it->sock.write_int16(CMD_OK); // Confirmação de login padrão
                      }
                  } else {
                      // Senha incorreta
                      new_sock.write_int16(CMD_ERROR);
                      new_sock.close();
                  }
              } else {
                  // Usuário não encontrado
                  new_sock.write_int16(CMD_ERROR);
                  new_sock.close();
              }
          } catch (const std::exception &e) {
              cerr << "Erro na conexão com o cliente: " << e.what() << endl;
              // Garante que o socket temporário seja fechado em caso de erro
              // para evitar vazamento de recursos.
              try {
                  if (new_sock.connected()) {
                      new_sock.close();
                  }
              } catch (...) {
                  cerr << "Erro ao fechar socket em caso de exceção" << endl;
              }
          }
      }



        }

      // De acordo com o resultado da espera:
      // SOCK_TIMEOUT:
      // Saiu por timeout: nao houve atividade em nenhum socket
      // Aproveita para salvar dados ou entao nao faz nada
      // SOCK_ERROR:
      // Erro no select: encerra o servidor
      // SOCK_OK:
      // Houve atividade em algum socket da fila:
      //   Testa se houve atividade nos sockets dos clientes. Se sim:
      //   - Leh o comando
      //   - Executa a acao
      //   = Envia resposta
      //   Depois, testa se houve atividade no socket de conexao. Se sim:
      //   - Estabelece nova conexao em socket temporario
      //   - Leh comando, login e senha
      //   - Testa usuario
      //   - Se deu tudo certo, faz o socket temporario ser o novo socket
      //     do cliente e envia confirmacao

    catch(const char* err)  // Erros mais graves que encerram o servidor
    {
      cerr << "Erro no servidor: " << err << endl;

      // Sai do while e encerra a thread
      server_on = false;

      // Fecha todos os sockets dos clientes
      for (auto& U : LU) U.close();
      // Fecha o socket de conexoes
      sock_server.close();
      break;

      // Os tanques continuam funcionando
    } // fim catch - Erros mais graves que encerram o servidor
  } // fim while (server_on)
}