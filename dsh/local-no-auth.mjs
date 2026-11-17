// Local-only DSH plugin: trust loopback requests without DSH's launch token.
// DSH binds 127.0.0.1, so only local processes can reach this listener. The
// public panel proxy still authenticates and authorizes every remote request.
export const name = 'local-no-auth'
export const inject = ['connection']

export function apply(ctx) {
  const connection = ctx.connection

  // Keep the Host/Origin fence, but do not require a browser cookie or API
  // token. `browserPeer` is DSH's owner principal, so the GUI behaves exactly
  // as it does after its normal launch-token exchange.
  connection.admit = (req) => {
    if (!connection.isTrusted(req)) return { rejection: 403 }
    if (connection.disposed) return { rejection: 401 }
    return { peer: connection.browserPeer }
  }

  // The root page is served for the same loopback-only listener.
  connection.authorizeIndex = () => true
}
