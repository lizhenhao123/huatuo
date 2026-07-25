# Tencent Lighthouse deployment

This deployment runs Elasticsearch, Grafana, huatuo-apiserver, and one
huatuo-bamai Agent on a small Lighthouse instance. Services bind their HTTP
ports to loopback. Use an SSH tunnel for Grafana:

```bash
ssh -L 3000:127.0.0.1:3000 ubuntu@SERVER
```

## Server bootstrap

Create the deployment directory once:

```bash
sudo install -d -m 0755 /opt/huatuo/releases
sudo install -m 0600 deploy/lighthouse/.env.example /opt/huatuo/.env
sudoedit /opt/huatuo/.env
```

Use immutable image tags. Generate all three secrets with
`openssl rand -hex 32`. Never commit `/opt/huatuo/.env`.

## Deploy or roll back

Place a repository checkout or archive under
`/opt/huatuo/releases/REVISION`, then run:

```bash
sudo /opt/huatuo/releases/REVISION/deploy/lighthouse/deploy.sh \
  /opt/huatuo/releases/REVISION
```

The script validates configuration, pulls images, waits for service health,
and atomically updates `/opt/huatuo/current`. If startup fails, it attempts
to restore the previous release.

To roll back manually, set `HUATUO_IMAGE_TAG` in `/opt/huatuo/.env` to the
previous immutable tag and run the previous release's `deploy.sh`.

## GitHub deployment environment

Create a protected GitHub environment named `lighthouse-production` and
require approval. Configure:

- Variables: `TENCENTCLOUD_REGION`, `LIGHTHOUSE_INSTANCE_ID`,
  `LIGHTHOUSE_TAT_COMMAND_ID`
- Secrets: `TENCENTCLOUD_SECRET_ID`, `TENCENTCLOUD_SECRET_KEY`

Install the fixed deployment helper on the server:

```bash
sudo install -d -m 0755 /opt/huatuo/bin
sudo install -m 0755 deploy/lighthouse/tat-deploy.sh \
  /opt/huatuo/bin/tat-deploy
```

Create one parameter-enabled TAT command in the same TencentCloud region:

```bash
/opt/huatuo/bin/tat-deploy "{{revision}}" "{{image_tag}}"
```

Run it as `ubuntu`, set a 1200-second timeout, and store its command ID in
`LIGHTHOUSE_TAT_COMMAND_ID`. The TencentCloud identity only needs
`InvokeCommand`, `DescribeInvocations`, and `DescribeInvocationTasks` for
that command and the target instance. Deny `RunCommand`, `CreateCommand`,
and `ModifyCommand`; the CI identity must not be able to execute arbitrary
scripts. The `deploy Lighthouse` workflow accepts a commit on `main` and an
immutable image tag, then invokes the fixed command with those parameters.
