#ifndef _CONFIG_H_
#define _CONFIG_H_
// clang-format off
#include <string>
#include <vector>
#include <memory>
// clang-format on
enum UserInfoDataState {
  USERNAME,
  PASSWORD,
  OPTIONAL,
  LOADBALANCE_QUOTA,
  LOADBALANCE_PRIORITY,
  MAX_VALUE
};

namespace EnumOps {
  // ADL helper.  See #define below for macro that writes
  // the "this enum should use enum ops" overload:
  template<class T>
  std::false_type use_enum_ops_f(T&&){return {};}

  // trait class that detects if we should be messing with this enum:
  template<class T>
  using use_enum_ops = decltype(use_enum_ops_f( std::declval<T>() ));

  // to-from underlying type:
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  constexpr std::underlying_type_t<E> get_underlying(E e) {
    return static_cast<std::underlying_type_t<E>>(e);
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  constexpr E from_underlying(std::underlying_type_t<E> e) {
    return static_cast<E>(e);
  }

  // Clamps your Enum value from 0 to E::MAX_VALUE using modular arithmetic
  // You must include a MAX_VALUE in your enum.
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E clamp_max( std::underlying_type_t<E> e ) {
    constexpr auto max = get_underlying(E::MAX_VALUE);
    if (e < 0) {
      auto count = -(e-max+1)/max;
      e =  e + count*max;
    }
    return from_underlying<E>(e % max);
  }

  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E& operator+=( E& e, std::underlying_type_t<E> x ) {
    e= clamp_max<E>(get_underlying(e) + x);
    return e;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E& operator-=( E& e, std::underlying_type_t<E> x ) {
    e= clamp_max<E>(get_underlying(e) - x);
    return e;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E operator+( E e, std::underlying_type_t<E> x ) {
    return e+=x;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E operator+( std::underlying_type_t<E> x, E e ) {
    return e+=x;
  }
  // no int - enum permitted, but enum-int is:
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E operator-( E e, std::underlying_type_t<E> x ) {
    e -= x;
    return e;
  }
  // enum-enum returns the distance between them:
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  std::underlying_type_t<E> operator-( E lhs, E rhs ) {
    return get_underlying(lhs) - get_underlying(rhs);
  }
  // ++ and -- support:
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E& operator++( E& lhs ) {
    lhs += 1;
    return lhs;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E operator++( E& lhs, int ) {
    auto tmp = lhs;
    ++lhs;
    return tmp;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E& operator--( E& lhs ) {
    lhs -= 1;
    return lhs;
  }
  template<class E,
    std::enable_if_t< use_enum_ops<E>{}, int> =0
  >
  E operator--( E& lhs, int ) {
    auto tmp = lhs;
    --lhs;
    return tmp;
  }
}
static std::true_type use_enum_ops_f(UserInfoDataState){ return {}; }


struct UserInfo {
public:
  std::string USERNAME;
  std::string PASSWORD;
  int LOADBALANCE_QUOTA;
  int LOADBALANCE_PRIORITY;
  UserInfoDataState state;
  UserInfo(std::string USERNAME, std::string PASSWORD, int LOADBALANCE_QUOTA,
           int LOADBALANCE_PRIORITY, UserInfoDataState state)
      : USERNAME(USERNAME), PASSWORD(PASSWORD),
        LOADBALANCE_QUOTA(LOADBALANCE_QUOTA),
        LOADBALANCE_PRIORITY(LOADBALANCE_PRIORITY), state(state) {}
};
class Config {
public:
  static Config *getInstance();
  void LoadConfig();
  bool parseCommandLine(int argc, char **argv);

  std::vector<std::shared_ptr<UserInfo>> userInfo;

  std::string APPKEY;
  std::string MOUNTPOINT;
  std::string CACHEPATH;
  int fuseindex;

private:
  Config();
  std::string configFile;
  const static unsigned int STRINGSIZE = 250;
  char *getString(std::string prompt, bool isPassword);

  std::shared_ptr<UserInfo> next(UserInfoDataState);

  void check_variable_multi(int &, std::string, std::string);
  void check_variable_multi(std::string &, std::string, std::string);

  void check_variable(int &, std::string value, std::string name);
  void check_variable(std::string &, std::string value, std::string name);
};
#endif
