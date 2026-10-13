// Package scripts are staged before the dashboard bundle. Queue registrations
// until the bundle replaces this bootstrap with its bounded runtime API.
window.ForkMeshDashboard = window.ForkMeshDashboard || {
  _registrations: [],
  registerPage(pageId, initializer) {
    this._registrations.push([pageId, initializer]);
  },
};
