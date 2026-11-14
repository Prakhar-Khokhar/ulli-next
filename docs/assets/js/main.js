/**
 * ULLI-Next — USB-less Linux Installer Client Application
 * Handles: Theme toggle, mobile navigation, and back-to-top.
 */

(function () {
  "use strict";

  // ─── DOM Elements & State ──────────────────────────────────────────────────

  const THEME_STORAGE_KEY = "ulli-theme-preference";

  // ─── Theme Management ──────────────────────────────────────────────────────

  /**
   * Safely read from localStorage, returning null on security/quota errors
   * (e.g. private browsing with storage blocked).
   */
  function safeGetItem(key) {
    try {
      return localStorage.getItem(key);
    } catch (e) {
      return null;
    }
  }

  /**
   * Safely write to localStorage, returning false on security/quota errors.
   * The theme is still applied to the DOM even if persistence fails.
   */
  function safeSetItem(key, value) {
    try {
      localStorage.setItem(key, value);
      return true;
    } catch (e) {
      return false;
    }
  }

  function getPreferredTheme() {
    const stored = safeGetItem(THEME_STORAGE_KEY);
    if (stored === "dark" || stored === "light") {
      return stored;
    }
    return window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  }

  function applyTheme(theme) {
    document.documentElement.setAttribute("data-theme", theme);
    const themeBtn = document.getElementById("theme-toggle");
    if (!themeBtn) return;

    const isLight = theme === "light";
    themeBtn.setAttribute("aria-label", isLight ? "Switch to dark theme" : "Switch to light theme");
    themeBtn.setAttribute("title", isLight ? "Switch to dark theme" : "Switch to light theme");

    // Update SVG icon inside button
    themeBtn.innerHTML = isLight
      ? `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path></svg>`
      : `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="5"></circle><line x1="12" y1="1" x2="12" y2="3"></line><line x1="12" y1="21" x2="12" y2="23"></line><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"></line><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"></line><line x1="1" y1="12" x2="3" y2="12"></line><line x1="21" y1="12" x2="23" y2="12"></line><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"></line><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"></line></svg>`;
  }

  function toggleTheme() {
    const current = document.documentElement.getAttribute("data-theme") || getPreferredTheme();
    const next = current === "light" ? "dark" : "light";
    safeSetItem(THEME_STORAGE_KEY, next);
    applyTheme(next);
  }

  function initTheme() {
    const theme = getPreferredTheme();
    applyTheme(theme);

    const themeBtn = document.getElementById("theme-toggle");
    if (themeBtn) {
      themeBtn.addEventListener("click", toggleTheme);
    }

    // Listen for OS scheme change if user hasn't set explicit override
    window.matchMedia("(prefers-color-scheme: light)").addEventListener("change", (e) => {
      if (!safeGetItem(THEME_STORAGE_KEY)) {
        applyTheme(e.matches ? "light" : "dark");
      }
    });
  }

  // ─── Mobile Navigation ─────────────────────────────────────────────────────

  function initMobileNav() {
    const toggleBtn = document.getElementById("mobile-menu-toggle");
    const drawer = document.getElementById("mobile-drawer");
    if (!toggleBtn || !drawer) return;

    function openDrawer() {
      drawer.classList.add("open");
      toggleBtn.setAttribute("aria-expanded", "true");
      document.body.style.overflow = "hidden";
    }

    function closeDrawer() {
      drawer.classList.remove("open");
      toggleBtn.setAttribute("aria-expanded", "false");
      document.body.style.overflow = "";
    }

    toggleBtn.addEventListener("click", () => {
      const isOpen = drawer.classList.contains("open");
      if (isOpen) {
        closeDrawer();
      } else {
        openDrawer();
      }
    });

    // Close when clicking nav links
    drawer.querySelectorAll(".nav-link").forEach((link) => {
      link.addEventListener("click", closeDrawer);
    });

    // Close on Escape key
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && drawer.classList.contains("open")) {
        closeDrawer();
        toggleBtn.focus();
      }
    });
  }

  // ─── Back to top button ────────────────────────────────────────────────────

  function initBackToTop() {
    const btn = document.getElementById("back-to-top");
    if (!btn) return;

    btn.addEventListener("click", () => {
      window.scrollTo({ top: 0, behavior: "smooth" });
    });
  }

  // ─── Bootstrapping ─────────────────────────────────────────────────────────

  document.addEventListener("DOMContentLoaded", () => {
    initTheme();
    initMobileNav();
    initBackToTop();
  });
})();
