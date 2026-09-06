/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_battle.h — the attack animation.
 *
 *  ONE CARD IS ON SCREEN AT A TIME, AND THAT IS A PALETTE DECISION BEFORE IT
 *  IS A STAGING ONE.  A battle card is shown from DAT/BIG.CRD, which carries
 *  its own sixteen colours -- black, white, and fourteen fitted to that one
 *  painting -- so two cards cannot share the screen: the second would have to
 *  be dithered into the first one's palette and would come back as sludge.
 *
 *  So the attacker is presented alone, slides off to the LEFT, the palette
 *  changes while the screen is empty, and the defender enters from the RIGHT.
 *  The strike is then resolved on the defender's card: it is destroyed, it
 *  survives, or it counter-kills, in which case the attacker's palette and
 *  card come back for its own death.  The order is the one the PC-FX and MSX2
 *  presentations use; what is new here is the palette swap in the gap, which
 *  is what buys each card fourteen colours of its own.
 *
 *  The rules have already been applied by the time this starts -- Msx2_Attack
 *  resolves atomically -- so everything shown is read back out of g_duel's
 *  last_* record.  Nothing here can change the outcome.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_BATTLE_H
#define WAIFU_ATARIST_BATTLE_H

/* Start the animation for the attack g_duel just resolved.  Returns non-zero
 * if it took; zero means the last action was not an attack, or the art is not
 * available, and the caller should carry on as it did before. */
int  Atarist_BattleBegin(void);

/* Non-zero while the animation owns the screen.  The duel must not draw, step
 * the rules or read input while this is true. */
int  Atarist_BattleActive(void);

/* Advance by the vblanks the last frame took, and draw this one. */
void Atarist_BattleStep(int vblanks);

#endif /* WAIFU_ATARIST_BATTLE_H */
