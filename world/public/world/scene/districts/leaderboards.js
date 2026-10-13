import { makeActiveLeaderboardSign } from "../arrival.js";
import {
  LEADERBOARD_CARDS_WITH_OWN_PAINTER,
  WORLD_LEADERBOARD_BOARD_STUBS,
  activeLeaderboardTexture,
  leaderboardStatTexture,
  makeLeaderboardStatSign,
  makeReferralLeaderboardSign,
  makeSiteReferrerLeaderboardSign,
  rankedActiveLeaderboardMembers,
  rankedReferralRows,
  rankedSiteReferrerRows,
  referralLeaderboardTexture,
  siteReferrerLeaderboardTexture,
} from "../leaderboards.js";

export function functions({
  THREE,
  ctx,
  fns,
}) {
  const leaderboardCards = new Map();

  function addLeaderboardCard(id, sign) {
    const face = sign.userData.face;
    if (face) {
      face.userData.interactive = "leaderboard-card";
      face.userData.boardId = id;
      face.userData.leaderboardPanelTile = true;
      ctx.interactive.push(face);
    }
    leaderboardCards.set(id, { id, sign, face, key: "" });
    fns.placeBillboardOnIsland(sign, {
      circle: "leaderboards",
      feed: "leaderboards",
    });
  }
  // Each card keys its own repaint, so a snapshot that only moved one board
  // rebuilds one 768² canvas rather than the whole circle.
  function repaintLeaderboardCard(id, key, paint) {
    const card = leaderboardCards.get(id);
    if (!card?.face?.material || card.key === key) return;
    card.key = key;
    card.face.material.map?.dispose?.();
    card.face.material.map = paint();
    card.face.material.needsUpdate = true;
  }

  function repaintActiveLeaderboardCard() {
    const rows = rankedActiveLeaderboardMembers(ctx.leaderboardCardState.members);
    repaintLeaderboardCard("active-members", JSON.stringify(rows), () =>
      activeLeaderboardTexture(THREE, ctx.leaderboardCardState.members),
    );
  }

  function updateLeaderboards(boards = []) {
    (Array.isArray(boards) ? boards : [])
      .filter((board) => board && typeof board === "object")
      .forEach((board) => {
        const id = String(board.id || "");
        if (!id || !leaderboardCards.has(id)) return;
        if (LEADERBOARD_CARDS_WITH_OWN_PAINTER.has(id)) return;
        ctx.leaderboardCardState.boards.set(id, board);
        repaintLeaderboardCard(id, JSON.stringify(board), () =>
          leaderboardStatTexture(THREE, board),
        );
      });
  }

  function updateReferralLeaderboard(rows = [], viewerLink = "") {
    ctx.leaderboardCardState.referralRows = Array.isArray(rows) ? rows : [];
    ctx.leaderboardCardState.viewerLink = String(viewerLink || "");
    repaintLeaderboardCard(
      "referrals",
      JSON.stringify([
        rankedReferralRows(ctx.leaderboardCardState.referralRows),
        ctx.leaderboardCardState.viewerLink,
      ]),
      () =>
        referralLeaderboardTexture(
          THREE,
          ctx.leaderboardCardState.referralRows,
          ctx.leaderboardCardState.viewerLink,
        ),
    );
  }
  function updateSiteReferrerLeaderboard(rows = [], totals = {}) {
    ctx.leaderboardCardState.siteRows = rankedSiteReferrerRows(rows);
    ctx.leaderboardCardState.siteTotals =
      totals && typeof totals === "object" ? totals : {};
    repaintLeaderboardCard(
      "http-referrers",
      JSON.stringify([
        ctx.leaderboardCardState.siteRows,
        ctx.leaderboardCardState.siteTotals,
      ]),
      () =>
        siteReferrerLeaderboardTexture(
          THREE,
          ctx.leaderboardCardState.siteRows,
          ctx.leaderboardCardState.siteTotals,
        ),
    );
  }

  return {
    addLeaderboardCard,
    repaintActiveLeaderboardCard,
    updateLeaderboards,
    updateReferralLeaderboard,
    updateSiteReferrerLeaderboard,
  };
}

export function build({
  THREE,
  addLeaderboardCard,
}) {
  // Every public ranking is a card of its own again, standing on the
  // leaderboard circle. The single 5×5 wall packed the whole catalog into one
  // 2048² texture, so a change to any board repainted all of them and there
  // was one object to walk up to instead of fifteen.
  const leaderboardCardState = {
    members: [],
    referralRows: [],
    viewerLink: "",
    siteRows: [],
    siteTotals: {},
    boards: new Map(
      WORLD_LEADERBOARD_BOARD_STUBS.map((board) => [board.id, board]),
    ),
  };
  addLeaderboardCard("active-members", makeActiveLeaderboardSign(THREE));
  addLeaderboardCard("referrals", makeReferralLeaderboardSign(THREE));
  addLeaderboardCard("http-referrers", makeSiteReferrerLeaderboardSign(THREE));
  for (const board of WORLD_LEADERBOARD_BOARD_STUBS) {
    addLeaderboardCard(board.id, makeLeaderboardStatSign(THREE, board));
  }
  return {
    leaderboardCardState,
  };
}
