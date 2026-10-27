// JPRF reader + @TP: recipe blob, checked against makefu/jura-connect vectors that were
// verified by physically brewing (S8 EB cafe barista, E6 espresso, E6 coffee).
#include "../components/jura_link/jura_jprf.h"
#include "../components/jura_link/jura_profiles_blob.h"
#include <cstdio>
#include <string>
using namespace jura_jprf;
static std::string hex(const uint8_t *b){ std::string s; char t[3]; for(int i=0;i<16;++i){snprintf(t,3,"%02X",b[i]); s+=t;} return s; }
static int fails=0;
static void check(const char *ef, uint8_t code, int32_t req[ATTR_COUNT], const char *want, const char *label){
  Profiles p(JURA_PROFILES,JURA_PROFILES_LEN); if(!p.select(ef)){printf("FAIL select %s\n",ef);++fails;return;}
  for(uint8_t i=0;i<p.product_count();++i){ Product pr=p.product(i); if(pr.code!=code) continue;
    uint8_t b[16]; recipe_blob(p,pr,req,b); std::string h=hex(b);
    bool ok = h==want; if(!ok) ++fails;
    printf("%s %-7s %-26s %s%s%s\n", ok?"ok  ":"FAIL", ef, pr.name, h.c_str(), ok?"":"  want ", ok?"":want); (void)label; return; }
  printf("FAIL no product %02X in %s\n",code,ef); ++fails;
}
int main(){
  Profiles all(JURA_PROFILES,JURA_PROFILES_LEN); printf("blob ok=%d machines=%d\n",all.ok(),all.machine_count());
  int maxp=0; // every machine selectable, product counts within cache
  for(uint16_t i=0;i<all.machine_count();++i){ Profiles q(JURA_PROFILES,JURA_PROFILES_LEN);
    const char* ef=q.str(rd16(JURA_PROFILES+12+i*IDX_SZ)); if(!q.select(ef)){printf("FAIL select %s\n",ef);++fails;}
    if(JURA_PROFILES[rd32(JURA_PROFILES+12+i*IDX_SZ+4)]>maxp) maxp=JURA_PROFILES[rd32(JURA_PROFILES+12+i*IDX_SZ+4)];
    if(q.maint_counter_count()>8||q.maint_pct_count()>8){printf("FAIL maint>8 %s\n",ef);++fails;} }
  printf("max products per machine = %d (cache %d)\n",maxp,MAX_PRODUCTS);
  int32_t d[ATTR_COUNT]; for(auto &x:d) x=-1;
  check("EF536M V02.01",0x02,d,"02000809000002000100000000000000","S8 espresso default (== E6 espresso vector)");
  int32_t cb[ATTR_COUNT]; for(auto &x:cb) x=-1; cb[COFFEE_STRENGTH]=7; cb[WATER_AMOUNT]=45; cb[TEMPERATURE]=1; cb[BYPASS]=45;
  check("EF1091",0x28,cb,"28000709000001000109000000000000","S8 EB cafe barista");
  int32_t e6[ATTR_COUNT]; for(auto &x:e6) x=-1; e6[COFFEE_STRENGTH]=2; e6[WATER_AMOUNT]=130; e6[TEMPERATURE]=1;
  check("EF1030",0x03,e6,"0300021A000001000100000000000000","E6 coffee s2 130ml normal");
  // clamping/snapping: 999 ml -> max 80, 47 ml -> 45
  int32_t cl[ATTR_COUNT]; for(auto &x:cl) x=-1; cl[WATER_AMOUNT]=999;
  check("EF536",0x02,cl,"02000810000002000100000000000000","clamp to 80 ml");
  cl[WATER_AMOUNT]=47; check("EF536",0x02,cl,"02000809000002000100000000000000","snap 47->45");
  cl[WATER_AMOUNT]=-1; cl[TEMPERATURE]=7; check("EF536",0x02,cl,"02000809000002000100000000000000","bad temp item -> default");
  int32_t lm[ATTR_COUNT]; for(auto &x:lm) x=-1;
  check("EF536",0x07,lm,"070008091600020001001E0000000000","latte macchiato defaults (milk 22 s, break 30 s)");
  printf(fails?"%d FAILURES\n":"ALL PASS\n",fails); return fails!=0;
}
